# SMMU v3 脏位追踪（dirty tracking）分析

> 文档面向 OLK-6.6（基于 Linux 6.6 LTS 的下游分支）的 iommufd 与 ARM SMMU v3
> 实现，梳理脏位追踪的数据结构、ioctl 入口、分配能力检查、硬件 HTTU 基础、
> 读取并清除脏位的遍历逻辑，以及 CONT 与 NO_CLEAR 的特殊处理。所有本地行号
> 均对应本仓库当前代码。源码根目录：`/home/chenjun/Git/OLK-6.6`。本文为静态
> 源码分析，未进行硬件实测，不改任何内核源码或配置。
>
> 相关文档：CPU 侧 CONT 位与脏位机制见 [CONT_BIT分析.md](CONT_BIT分析.md)；
> 并存的 VFIO type1 接口见 [dirty_log分析.md](dirty_log分析.md)，本文末尾说明二者关系。
> 注意脏位追踪（dirty tracking）与 tracepoint 意义上的 dirty tracing 无关。

## 1. 范围与核心对象

只覆盖 `domain->dirty_ops` 这套接口（`set_dirty_tracking` /
`read_and_clear_dirty`），以及 iommufd 暴露给用户态的 IOAS / HWPT 中与脏位
相关的部分。涉及的源文件：`drivers/iommu/iommufd/{hw_pagetable.c,
io_pagetable.c,iova_bitmap.c}`、`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c`、
`drivers/iommu/io-pgtable-arm.c`、`include/linux/iommu.h`、
`include/uapi/linux/iommufd.h`，关键行号在下文各节内联标注。

## 2. 数据结构与标志位

`struct iommu_dirty_ops`（`iommu.h:471`）只有两个回调：`set_dirty_tracking`
与 `read_and_clear_dirty`。域挂指针 `domain->dirty_ops`（`iommu.h:258`），
为 NULL 即不支持，调用方据此返回 `-EOPNOTSUPP`（`io_pagetable.c:493`、`:597`）。
`read_and_clear_dirty` 的 `flags` 只定义 `IOMMU_DIRTY_NO_CLEAR`（`1<<0`，
`iommu.h:397`）。

| uapi 标志 | 值 | 用途 | 定义 |
|---|---|---|---|
| `IOMMU_HWPT_ALLOC_DIRTY_TRACKING` | `1<<1` | 分配 HWPT 即要求脏位追踪能力 | `iommufd.h:372` |
| `IOMMU_HWPT_DIRTY_TRACKING_ENABLE` | `1` | `SET_DIRTY_TRACKING` 开关位 | `iommufd.h:674` |
| `IOMMU_HWPT_GET_DIRTY_BITMAP_NO_CLEAR` | `1` | `GET_DIRTY_BITMAP` 不清除 | `iommufd.h:705` |

`IOMMU_DIRTY_NO_CLEAR`（内核态）由 `hw_pagetable.c:499` 经 `cmd->flags` 从
`IOMMU_HWPT_GET_DIRTY_BITMAP_NO_CLEAR` 透传到 iopt 层再到遍历逻辑。

## 3. 分配时的能力检查与域注册

`iommufd_hwpt_paging_alloc`（`hw_pagetable.c:107`）校验有效标志集合含
`IOMMU_HWPT_ALLOC_DIRTY_TRACKING`（`:112-113`），带该标志多做能力检查：
未达 `IOMMU_CAP_DIRTY_TRACKING` 返回 `-EOPNOTSUPP`（`:126-128`）。SMMU v3
对此能力实现为 `arm_smmu_dbm_capable`，要求同时具有 `ARM_SMMU_FEAT_HD` 和
`ARM_SMMU_FEAT_COHERENCY`（`arm-smmu-v3.c:2562-2566`、`:2583-2584`）。
这比另一套 `support_dirty_log` 仅检查 HD 的条件更严格，不能混用二者的能力判断。

`arm_smmu_domain_finalise`（`arm-smmu-v3.c:2715`）关键分支：

```c
bool enable_dirty = flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING;       /* :2724 */
case S1: if (enable_dirty) quirks |= IO_PGTABLE_QUIRK_ARM_HD;       /* :2755 */
case S2: if (enable_dirty) return -EOPNOTSUPP;                       /* :2763 */
if (smmu->features & FEAT_HD) quirks |= IO_PGTABLE_QUIRK_ARM_HD;  /* :2783 */
if (enable_dirty && stage==S1) domain->dirty_ops = &arm_smmu_dirty_ops;  /* :2801 */
```

三点事实：脏位追踪只支持 Stage-1，Stage-2 带标志直接返回 `-EOPNOTSUPP`（`:2763`），
Stage-2 细节本文不展开；`dirty_ops` 仅在显式带分配标志且 S1 时挂上（`:2801-2802`）；
即便不带分配标志，只要硬件有 `FEAT_HD`，`IO_PGTABLE_QUIRK_ARM_HD` 仍被设置（`:2783`），
S1 可写映射 PTE 会带 DBM（见下节），但 `dirty_ops` 为 NULL，
`iommu_read_and_clear_dirty` 因 ops 为空返回 `-EOPNOTSUPP`（`io_pagetable.c:493`）。
即底层配置和软件接口是否暴露是两个层面。此处代码块为分支摘要伪代码，非可编译源码。

## 4. 硬件基础：HTTU 的 HA / HD 与 PTE 脏位

SMMU v3 的 HTTU 从 `IDR0.HTTU` 读取，`IDR0_HTTU_ACCESS` 对应 HA（硬件 Access
flag），`IDR0_HTTU_ACCESS_DIRTY` 对应 HD（硬件 Dirty）（`arm-smmu-v3.c:5198-5203`）。
脏位追踪依赖 HD；硬件支持还须配合启用配置。`arm_smmu_make_s1_cd` 在 HD quirk
存在时同时设置 Context Descriptor 的 HA/HD（`arm-smmu-v3.c:1611-1614`）。
PTE 位定义（`io-pgtable-arm.c`）：`ARM_LPAE_PTE_DBM`=bit51
（`:78`），`ARM_LPAE_PTE_AP_RDONLY`=bit7（AP[2]，`:109`），
`ARM_LPAE_PTE_AP_WR_CLEAN_MASK`=DBM|AP[2]（`:111-112`）。

判定与修改宏（`:170-174`）：`iopte_writeable_dirty(pte)` 即
`(pte & AP_WR_CLEAN_MASK) == DBM`，命中 DBM=1 且 AP[2]=0；`iopte_set_writeable_clean(ptep)`
即 `set_bit(AP_RDONLY_BIT, ptep)` 置 AP[2]=1。DBM=1 时的脏位约定：

| DBM | AP[2] | `iopte_writeable_dirty` | 语义 |
|---|---|---|---|
| 1 | 1 | false | 可写干净（writable-clean） |
| 1 | 0 | true | 可写脏（writable-dirty） |
| 0 | 任意 | false | 非 HD 映射，不参与追踪 |

状态机：软件 `iopte_set_writeable_clean` 置 AP[2]=1 回到干净，DMA 写硬件清
AP[2]→0 变脏，`read_and_clear_dirty` 命中脏态后清回干净。

```
 writable-clean (DBM=1,AP2=1)  --DMA写,HW清AP2-->  writable-dirty (DBM=1,AP2=0)
        ^                                                   │
        └────────────── read_and_clear_dirty 命中 ──────────┘
```

映射建立时（`io-pgtable-arm.c:533-535`）：S1 + HD quirk + 可写映射会 `pte |= DBM`，
AP[2] 默认 0，即映射出来即 writable-dirty 形态。干净基线由 `iopt_set_dirty_tracking`
开启时的清空动作建立（见第 6 节）。SMMU 旧式 dirty_log 与本文 dirty_ops 共享同一套
DBM/AP[2] 物理 PTE（见第 9 节）。

## 5. ioctl 调用链

两个 ioctl 入口在 `hw_pagetable.c`，经 IOAS 间接操作域：

```
IOMMU_HWPT_SET_DIRTY_TRACKING
  iommufd_hwpt_set_dirty_tracking (hw_pagetable.c:457)
   └ iopt_set_dirty_tracking (io_pagetable.c:591)
       ├ (enable) iopt_clear_dirty_data (io_pagetable.c:562)
       └ ops->set_dirty_tracking -> arm_smmu_set_dirty_tracking (smmu-v3.c:3764)  no-op, return 0

IOMMU_HWPT_GET_DIRTY_BITMAP
  iommufd_hwpt_get_dirty_bitmap (hw_pagetable.c:482)
   └ iopt_read_and_clear_dirty_data (io_pagetable.c:544)
       └ iommu_read_and_clear_dirty (io_pagetable.c:482)
           └ iova_bitmap_for_each (iova_bitmap.c:415)
               └ __iommu_read_and_clear_dirty (io_pagetable.c:452)
                   └ ops->read_and_clear_dirty -> arm_smmu_read_and_clear_dirty (smmu-v3.c:3753)
                       └ arm_lpae_read_and_clear_dirty (io-pgtable-arm.c:1381)
                           └ __arm_lpae_iopte_walk_dirty (:1355) -> io_pgtable_visit_dirty (:1325)
```

`hw_pagetable.c:465`、`:489` 各做一次标志合法性过滤，多余标志直接
`-EOPNOTSUPP`。

## 6. 设置追踪：先清后开，硬件层是 no-op

`iopt_set_dirty_tracking`（`io_pagetable.c:591`）在 `enable` 为真时先调用
`iopt_clear_dirty_data`（`:604`），再调用 `ops->set_dirty_tracking`（`:609`）。

`iopt_clear_dirty_data`（`:562`）用空位图初始化
（`iommu_dirty_bitmap_init(&dirty, NULL, &gather)`，`:573`），遍历所有 area 对
每段调用 `ops->read_and_clear_dirty`（`flags=0`，`:580`），末尾
`iommu_iotlb_sync(domain, &gather)`（`:587`）。此处 `dirty->bitmap` 为 NULL、
`dirty->gather` 指向本地 gather；回调里 `iommu_dirty_bitmap_record`
（`iommu.h:1246`）在 `bitmap` 为 NULL 时只做 `iommu_iotlb_gather_add_range`
（`iommu.h:1253-1254`）。因此遇到脏项时会记录 gather 的地址范围，但这不能证明
后续 sync 真正发出失效命令，详见第 7 节的 `pgsize` 限制。

`arm_smmu_set_dirty_tracking`（`arm-smmu-v3.c:3764`）直接返回 0，注释说明
硬件始终开启，脏位已在 `set_dirty_tracking` 之前清空（`:3767-3770`）。它不是
硬件开关：硬件 HTTU DBM 在映射建立时已生效，软件“开启”的真实动作是清空脏位
基线，之后硬件写会重新置脏。

## 7. 读取并清除脏位

`iopt_read_and_clear_dirty_data`（`io_pagetable.c:544`）先做 IOVA 范围与对齐校验
（`:551`），再持读锁调用 `iommu_read_and_clear_dirty`（`:556`）。后者（`:482`）分配
`iova_bitmap`（`:496`）并用真实位图初始化（`:502`），随后遍历并按需 sync：

```c
iova_bitmap_for_each(iter, &arg, __iommu_read_and_clear_dirty);  /* :508 */
if (!(flags & IOMMU_DIRTY_NO_CLEAR)) iommu_iotlb_sync(domain, &gather);  /* :511 */
```

`iova_bitmap_for_each`（`iova_bitmap.c:415`）按页切用户位图为 IOVA 段回调。
`__iommu_read_and_clear_dirty`（`io_pagetable.c:452`）再用 `iopt_for_each_contig_area`
（`:466`）按 iopt 连续区切分，对每段调 `ops->read_and_clear_dirty`（`:469`）。

到 io-pgtable 层，`arm_lpae_read_and_clear_dirty`（`io-pgtable-arm.c:1381`）
限定 `fmt == ARM_64_LPAE_S1`（`:1401`），否则 `-EINVAL`。叶子由
`io_pgtable_visit_dirty`（`:1325`）处理，命中 `iopte_writeable_dirty`（`:1335`）
时：CONT 脏位 + 非 NO_CLEAR 返回 `-EOPNOTSUPP`（`:1336-1338`）；否则
`iommu_dirty_bitmap_record(dirty, addr, size)`（`:1339`），并在非 NO_CLEAR 时
`iopte_set_writeable_clean(ptep)`（`:1341`）清回干净。

脏位记录粒度是叶子 PTE 覆盖的块 / 页大小 `ARM_LPAE_BLOCK_SIZE(lvl)`（`:1333`），
即 4K/64K/2M 等映射粒度之一，并非统一子页粒度，也不保证原子地反映某瞬间的全部
DMA 写。`iommu_dirty_bitmap_record` 同时写用户位图（`iova_bitmap_set`）与 gather
（`iommu_iotlb_gather_add_range`）；默认模式在遍历后调用 `iommu_iotlb_sync`，
其设计目的在于让后续写重新观察 clean 状态。**当前版本不能仅凭该调用保证刷新生效：**
gather 初始化后 `pgsize=0`（`iommu.h:1025`），`add_range` 仅更新 start/end，
不设置 pgsize（`:1191-1199`）；上述 SMMU 遍历也未补充它。而
`arm_smmu_iotlb_sync` 在 `pgsize==0` 时直接返回（`arm-smmu-v3.c:3533-3534`）。
因此在这里分析的这条干净初始化 / 读清调用链上，存在“收集了范围并调用 sync，
但未实际下发 TLBI”的源码衔接问题。本文仅记录静态结果，不修改实现或宣称已验证修复。

## 8. CONT 与 NO_CLEAR 的特殊处理

第 7 节 `:1336-1338` 是 CONT 的硬限制：叶子 PTE 带 `ARM_LPAE_PTE_CONT`
（`io-pgtable-arm.c:97`，bit 52）、处于 writable-dirty、且未带 `NO_CLEAR`，
遍历在该层直接返回 `-EOPNOTSUPP`。本路径未实现 CPU contpte 那样的整组状态管理，
不能直接套用普通叶子清除逻辑；也不能简单解释为“CONT 组中 AP[2] 不同就必定违规”，
架构明确允许硬件更新导致组内差异。CONT 语义见 [CONT_BIT分析.md](CONT_BIT分析.md)。
带 `NO_CLEAR` 时该拒绝分支不触发，只记录不清除；仍不保证精确的组内基础页写入记录。

| 模式 | 标志 | 行为 | 适用 |
|---|---|---|---|
| 默认（读并清） | 无 `NO_CLEAR` | 记录后置 AP[2]=1，并调用 sync，实际限制见第 7 节 | 意图用于增量轮询 |
| 只读不清 | `GET_DIRTY_BITMAP_NO_CLEAR` | 仅记录，PTE 保持脏，不清不 sync | 累计观察或先看再决定清除 |

“默认每轮重置”指不带 `NO_CLEAR` 的 `GET_DIRTY_BITMAP` 会清掉已记录的脏位，
在正确完成清除和所需同步、映射稳定且 bitmap 输出基线正确的前提下，可用于观察
下一轮新脏状态。当前代码不能忽略第 7、10 节的限制而作此保证。只读模式因不调用
`iopte_set_writeable_clean`（`:1341` 条件为假）而不重置，脏位累积，需调用方
自行安排清除时机。

## 9. 与 dirty_log 并行 API 的关系

SMMU v3 在 `#ifdef CONFIG_ARM_SMMU_V3_HTTU`（`arm-smmu-v3.c:4264`）下另挂一套
`iommu_domain_ops` 风格脏日志接口：`support_dirty_log`（`:3896`，看 FEAT_HD）、
`switch_dirty_log`（`:3903`）、`sync_dirty_log`、`clear_dirty_log`，是 VFIO type1
旧路径用的，与本文 `iommu_dirty_ops`（`:4274-4277`）是两套独立接口，但底层都依赖
`ARM_SMMU_FEAT_HD`、操作同一套 DBM/AP[2] 物理 PTE 脏位。两套无代码依赖，
`set_dirty_tracking` 不要求先调 `switch_dirty_log`，反之亦然；用哪套取决于用户态入口
（iommufd ioctl 还是 VFIO type1）。另一套接口详见 [dirty_log分析.md](dirty_log分析.md)。
两套会清除同一份页表脏状态，不应据此推断可并发混用而互不影响。

## 10. 错误传播的注意事项

`iova_bitmap_for_each`（`iova_bitmap.c:415`）本身会传播回调错误：回调返回非零则
`break` 并把该值作为返回值（`:420-425`）。回调 `__iommu_read_and_clear_dirty` 也会在
`ops->read_and_clear_dirty` 失败时返回错误（`io_pagetable.c:472-473`），例如第 8 节
CONT 脏位的 `-EOPNOTSUPP` 或 `:477` 的 `-EINVAL`。

但当前版本 `iommu_read_and_clear_dirty` 内部 `ret=0`（`io_pagetable.c:491`），调用
`iova_bitmap_for_each(iter, &arg, __iommu_read_and_clear_dirty)`（`:508`）时未
捕获其返回值，完成遍历后的正常尾部 `return ret`（`:515`）仍为 0，
并非说前面的能力 / 分配检查也不会失败。因此底层遍历回调返回的错误
（含 CONT 脏位的 `-EOPNOTSUPP`）不会经此函数传到 `iopt_read_and_clear_dirty_data`
与 ioctl。本版本此调用未接收遍历返回值，底层错误与上层传播需分别看。本文仅记录
该事实，不做修改，也不据此推断全局不可用。`iopt_clear_dirty_data`（`:580-584`）
则正常捕获并 `break`，错误能传到 `iopt_set_dirty_tracking`。

## 11. 小结

链路为用户态 ioctl（`hw_pagetable.c:457/482`）到 iopt 层
（`io_pagetable.c:591/544`）到 `iommu_dirty_ops`（`iommu.h:471`），SMMU v3 在
`arm_smmu_dirty_ops`（`arm-smmu-v3.c:4274`）注册，最终由 io-pgtable 的
`io_pgtable_visit_dirty`（`io-pgtable-arm.c:1325`）遍历 S1 PTE 的 DBM/AP[2]。硬件层
`set_dirty_tracking` 是 no-op，开启会先尝试清空脏位基线；读取默认清除并调用同步，
但必须同时考虑 gather 的 pgsize 和错误返回值衔接问题。CONT 脏项在底层清除分支
返回 `-EOPNOTSUPP`，不能无条件说该错误会到达 ioctl；NO_CLEAR 则绕过该拒绝分支。
本文未做硬件实测，未修改任何源码或配置。
