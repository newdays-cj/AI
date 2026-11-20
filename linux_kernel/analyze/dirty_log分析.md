# dirty_log 脏页日志分析

源码根目录：`/home/chenjun/Git/OLK-6.6`
分析对象：`iommu_domain_ops` 中的 `support_dirty_log` / `switch_dirty_log` /
`sync_dirty_log` / `clear_dirty_log` 四件套，及其在 VFIO type1、ARM SMMU v3、
ARM LPAE io-pgtable 中的实现。

> 本文为源码静态分析，非运行时验证。所有结论基于当前仓库快照，不构成正确性证明，
> 也未参照上游提交历史。行号引用截至写作时该文件状态。

## 一、API 概览

dirty_log 是 IOMMU 域（`struct iommu_domain`）级别的一组脏页日志接口，定义在
`include/linux/iommu.h:836` 的 `iommu_domain_ops` 注释及 `:877`-`:887` 四个回调：

| 回调 | 声明位置 | 语义 |
|---|---|---|
| `support_dirty_log` | `iommu.h:877` | 查询域是否支持硬件脏页日志 |
| `switch_dirty_log` | `iommu.h:878` | 开启或停止脏页日志，参数 `enable/iova/size/prot` |
| `sync_dirty_log` | `iommu.h:880` | 把硬件脏状态同步进一张位图，不清脏 |
| `clear_dirty_log` | `iommu.h:884` | 按掩码位图清脏 |

`iommu.h:873`-`:876` 注释明确：这组接口不要与其它访问底层页表的 op 并发调用。
域结构体里专用的 `struct mutex switch_log_lock`（`iommu.h:283`）串行化 switch/sync/clear。

注意：函数名带 `trace_` 前缀的 `trace_switch_dirty_log` / `trace_sync_dirty_log` /
`trace_clear_dirty_log`（`include/trace/events/iommu.h:127/150/170`）是 tracepoint
插桩事件，仅记录调用，不是另一套独立的脏追踪需求。

## 二、调用链总览

```
用户态 VFIO ioctl
  VFIO_IOMMU_DIRTY_PAGES  (vfio_iommu_type1.c:3394)
   |
   +-- FLAG_START/STOP  -> vfio_dma_dirty_log_switch      (vfio_iommu_type1.c:1609)
   |                        -> iommu_switch_dirty_log    (iommu.c:2820)
   +-- FLAG_GET_BITMAP[_NOCLEAR] -> vfio_iommu_dirty_log_sync (vfio_iommu_type1.c:1367)
   |                               -> iommu_sync_dirty_log (iommu.c:2871)
   +-- FLAG_CLEAR_BITMAP -> vfio_iommu_dirty_log_clear     (vfio_iommu_type1.c:1346)
                            -> iommu_clear_dirty_log      (iommu.c:2955)

IOMMU 核心 (drivers/iommu/iommu.c)
  对齐检查 + 取 switch_log_lock + iommu_pgsize 分块 -> ops->xxx_dirty_log

ARM SMMU v3 (drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:3896-3991)
  switch: split_block / merge_page      sync: 预 flush 后转 io-pgtable
                                          clear: 直接转 io-pgtable
   |
   v
ARM LPAE io-pgtable (drivers/iommu/io-pgtable-arm.c)
  io_pgtable_ops.sync_dirty_log (:1173) / clear_dirty_log (:1285)
  split_block / merge_page 同文件另处
```

## 三、核心层 `drivers/iommu/iommu.c`

四个入口都在 `iommu.c:2812`-`:2991`。其中 support 只检查回调并查询能力，缺失时
返回 false，不执行下面的锁、对齐和分块操作。switch/sync/clear 的主要特征：

1. `ops->xxx_dirty_log` 为空时返回 `-ENODEV`（`:2830`/`:2881`/`:2929`）。
2. 对齐校验：取 `min_pagesz = 1 << __ffs(domain->pgsize_bitmap)`，要求
   `iova` 与 `size` 都按 `min_pagesz` 对齐，否则 `-EINVAL`（`:2833`/`:2884`/`:2965`）。
3. 加 `domain->switch_log_lock`。
4. 用 `iommu_pgsize(domain, iova, iova, size, NULL)`（`iommu.c:2448`）逐段取最大可
   用页大小，循环把请求切成块下发给驱动回调。

四个语义区分：

- `iommu_switch_dirty_log`（`:2820`）：循环内对每块调 `ops->switch_dirty_log`，
  任一块失败即跳出。循环后只要 `flush` 为真就 `iommu_flush_iotlb_all(domain)`
  （`:2860`），成功后 `trace_switch_dirty_log`（`:2864`）。
- `iommu_sync_dirty_log`（`:2871`）：逐块调 `ops->sync_dirty_log`，把硬件脏态写进
  调用方传入的 `bitmap`。核心层**不做 flush，也不自动清脏**，仅 `trace_sync_dirty_log`。
- `iommu_clear_dirty_log`（`:2955`）：清脏以调用方 `bitmap` 为**掩码**。核心用
  `for_each_set_bitrange_from(rs, re, bitmap, ...)`（`:2975`）只遍历置位段，对每个
  连续置位段调内部 `__iommu_clear_dirty_log`（`:2918`）。有清操作则
   `iommu_flush_iotlb_all`（`:2985`）；内部 helper 在每个成功处理范围后发出
   `trace_clear_dirty_log`。掩码语义即：只清
  位图标记的页，未标记的不动。
- `__iommu_clear_dirty_log`（`:2918`）：被外层在持锁情况下调用，仅做分块下发，不
  再取锁、不 flush。

## 四、VFIO type1 调用方 `drivers/vfio/vfio_iommu_type1.c`

VFIO 在 `struct vfio_domain` 上有 `bool iommu_hwdbm`（`:99`），由
`iommu_support_dirty_log`（`:2542`）在域接入时探测并写回（`:2541`-`:2554`）。
switch 会跳过 `iommu_hwdbm` 为假的域。读取还受 `num_non_pinned_groups`、
`dma->iommu_mapped`、`num_non_hwdbm_domains` 等条件控制；混有不能报告硬件脏页的
域时，相关分支保守地把整个范围标脏，而不是获得精确的硬件写记录（`:1468-1480`）。

ioctl 入口 `vfio_iommu_type1_dirty_pages`（`:3394`）分发：

- `FLAG_START`（`:3421`）：分配位图，置 `dirty_page_tracking`，调
  `vfio_iommu_dirty_log_switch(iommu, true)`（`:3430`）。
- `FLAG_STOP`（`:3435`）：`switch(false)`（`:3440`）并释放位图。
- `FLAG_GET_BITMAP` / `FLAG_GET_BITMAP_NOCLEAR`（`:3494`）：走
  `vfio_iova_dirty_bitmap`，内部 `update_user_bitmap`（`:1455`）在无非硬件脏域时调
  `vfio_iommu_dirty_log_sync`（`:1472`）同步；`NOCLEAR` 由 `dirty_log_get_no_clear`
   （`:1546-1548`）控制是否随后清脏。默认 GET 在满足硬件路径条件时另调用 clear
   （`:1550-1557`），这是 VFIO 组合两步，不是 sync 函数隐含清脏。
- `FLAG_CLEAR_BITMAP`（`:3504`）：走 `vfio_iova_dirty_log_clear`，用用户态位图作掩码
  清脏。

三个 VFIO 包装函数都在 `domain_list` 上遍历域（`:1355`/`:1374`/`:1617`）。
sync/clear 任一域失败会 `pr_warn` 并跳出；逐 DMA 的 switch 使用 `WARN_ON`
（`:1620`）报告失败，该 void 包装函数不会像 sync/clear 一样返回错误。

## 五、ARM SMMU v3 层 `arm-smmu-v3.c:3896-3991`

- `arm_smmu_support_dirty_log`（`:3896`）：仅看 `ARM_SMMU_FEAT_HD`。
- `arm_smmu_switch_dirty_log`（`:3903`）：要求 HD 且 `stage == ARM_SMMU_DOMAIN_S1`
  （`:3909`/`:3911`）。开启走 `arm_smmu_split_block`（`:3924`），停止走
  `arm_smmu_merge_page`（`:3934`）。代码注释指出：SMMU 硬件脏管理在支持 HTTU HD 时
   本就常开，开关日志的实际动作是拆块 / 合页。**通过 HD/S1 前置检查后，拆分或合并失败
   不向上传播，末尾仍返回 0**
  （`:3920`/`:3931`/`:3937`）：拆分失败仍可在块粒度记脏，合并失败只影响性能。
- `arm_smmu_split_block`（`:3792`）：要求 `BBML1|BBML2`（`:3800`），调
  `ops->split_block`，`handled_size != size` 时只记 `pr_err` 并返回 `-EFAULT`，
  该返回值被 `arm_smmu_switch_dirty_log` 丢弃。
- `arm_smmu_sync_dirty_log`（`:3940`）：转调 `ops->sync_dirty_log` 前**先做
  `arm_smmu_flush_iotlb_all`**（`:3964`），注释引用 IHI0070Da 3.13.4 "HTTU behavior
  summary"，确保在途事务完成。本层不自动清脏。
- `arm_smmu_clear_dirty_log`（`:3969`）：直接转 `ops->clear_dirty_log`，无预 flush。

## 六、ARM LPAE io-pgtable 层 `io-pgtable-arm.c`

PTE 字段：`ARM_LPAE_PTE_DBM` 为 bit 51（`:78`），`ARM_LPAE_PTE_AP_RDONLY` 为 bit 7
（`:109`），`AP_WR_CLEAN_MASK = AP_RDONLY | DBM`（`:111`）。脏判定
`iopte_writeable_dirty`（`:170-171`）= `(pte & AP_WR_CLEAN_MASK) == DBM`，即 DBM 置位且 AP_RDONLY
清零。建表时 `arm_lpae_prot_to_pte`（`:522`）：S1 只读页置 `AP_RDONLY`（`:532`），
S1 可写页且 `IO_PGTABLE_QUIRK_ARM_HD` 置 `DBM`（`:535`）。

`io_pgtable_ops` 中的 `sync_dirty_log`（`io-pgtable.h:239`）与 `clear_dirty_log`
（`:243`）由下面实现。

### sync：写位图，不清脏（`:1095`-`:1171`）

`__arm_lpae_sync_dirty_log` 递归走页表：
- 叶子且 `AP_RDONLY` 置位（干净）→ 返回 0，不写位图（`:1136`/`:1157`）。
- 叶子且可写 → `bitmap_set(bitmap, offset, nbits)`（`:1142`/`:1163`），**不修改 PTE**。
  `offset = (iova - base_iova) >> bitmap_pgshift`，`nbits = size >> bitmap_pgshift`。

sync 仅把“可写即视为脏”写进位图，不触碰硬件脏态，故需配合 clear 才能滚动周期。

### clear：掩码清脏（`:1201`-`:1283`）

`__arm_lpae_clear_dirty_log` 递归走页表：
- 叶子且 `AP_RDONLY` 置位 → 已干净，返回 0（`:1242`）。
- 叶子且 `PTE_CONT` 置位 → 返回 `-EOPNOTSUPP`（`:1244`-`:1245`）。这是脏页 clear 对
  CONT 映射的拒绝分支：含 CONT 的脏页不能按页清。
- 叶子可写非 CONT → 逐位检查 `bitmap` 对应范围，**只有全部置位才**置
  `pte |= ARM_LPAE_PTE_AP_RDONLY`（`:1250`-`:1257`）标记干净；任一位未置位则返回 0
   不清。当请求小于实际脏 block 叶子时，另一个分支返回 `-EINVAL`（`:1271-1276`），
   不能靠更细的 bitmap 粒度直接清除 block 内某个子页。

sync/clear 入口 `arm_lpae_sync_dirty_log`（`:1173`）/ `arm_lpae_clear_dirty_log`
（`:1285`）都要求 `ARM_64_LPAE_S1` 或 `ARM_32_LPAE_S1`（`:1193`/`:1305`）。

## 七、硬件 Stage1 脏位机制（DBM / AP[2]）

ARMv8 Stage1 在 HTTU HD 支持下，用 DBM（bit 51）+ AP[2]（bit 7）编码脏态：

| DBM | AP[2]（AP_RDONLY） | 状态 |
|---|---|---|
| 1 | 0 | writable-dirty，扫描会视为脏，不表示已写入软件位图 |
| 1 | 1 | 干净（read-only，HTTU 可在写时清 AP[2]） |

- 干净 → 脏：硬件在 HTTU 硬件更新开启时，写访问清 AP[2] 由 1 变 0，标记脏。
- 软件复位：`clear_dirty_log` 置 `AP_RDONLY`（AP[2]=1）回到干净。

注意：这里的“位图”是软件维护的脏页位图（`sync` 写入、`clear` 当掩码），**不是硬件
环形日志或硬件脏位图**。这里的硬件脏状态保存在页表叶子描述符中，可能对应 page
或 block，而非固定每个基础页各有独立状态。新建可写 HD 映射可能直接是 dirty 编码，
所以 dirty 也不是“确有一次 DMA 写”的严格证明；清脏后才建立新的观察基线。

## 八、位图含义与粒度

- `sync_dirty_log` 的 `bitmap`：输出位图，第 `i` 位 = `base_iova + (i << bitmap_pgshift)`
  对应页当前可写（视为脏）。`bitmap_pgshift` 由调用方指定，VFIO 用最小支持页的
   `__ffs(pgsize)`（`vfio_iommu_type1.c:1459`、`:1516`）。
- `clear_dirty_log` 的 `bitmap`：**输入掩码**，置位位表示“允许清这些页”。
- 粒度受 `iommu_pgsize` 分块与页表实际块大小共同约束；位图位宽
  `bitmap_pgshift` 不一定等于硬件最小页。

## 九、启停生命周期

| 阶段 | 动作 | 代码 |
|---|---|---|
| START | 尝试拆 block 为 page，提高记录粒度，不隐含清脏 | switch(enable=true) → split_block |
| 建立基线 | 用覆盖所需范围的掩码 clear；成功处理的叶子 AP[2]=1 | clear_dirty_log |
| 运行 | DMA 写触发硬件清 AP[2]=0 | 硬件 HTTU |
| 周期 2 | sync 读可写页进位图 | sync_dirty_log |
| 周期 3 | 按掩码 clear 复位 | clear_dirty_log |
| STOP | 尝试合 page 回 block，不保证合并成功 | switch(enable=false) → merge_page |

## 十、与 `dirty_tracking分析.md` 的关系

本仓库另有以 `read_and_clear_dirty`（`io-pgtable.h:231`）+ `iommu_dirty_bitmap` 为
核心的脏页路径，详见同目录 [dirty_tracking分析.md](dirty_tracking分析.md)。两者关系：

- **并行独立的 API**：dirty_log 走 `switch/sync/clear_dirty_log`，dirty_tracking 走
  `dirty_ops`/`read_and_clear_dirty`。本 dirty_log 路径**不依赖** `dirty_ops`。
- **共享底层机制**：两者最终都落到 Stage1 PTE 的 DBM/AP[2] 物理脏位，但 API 语义、
  位图模型、清脏时机不同。
- **不要推断**：不要假设两者有独立计数器，也不要假设两者可并发安全混用。
  `iommu.h:873` 已声明这组接口禁止与其它访问页表的 op 并发。

CONT 相关背景见同目录 [CONT_BIT分析.md](CONT_BIT分析.md)。

## 十一、局限

- **CONT**：clear 对含 `PTE_CONT` 的脏页返回 `-EOPNOTSUPP`（`io-pgtable-arm.c:1244`），
  即 dirty_log 的 clear 不保证能处理 CONT 映射。switch 通过 split_block 把块拆成页以
  消除块粒度问题，但 split/merge 失败被静默吞掉（`arm-smmu-v3.c:3920`/`:3931`），
  实际是否拆到页粒度无强保证。
- **block vs page**：sync 对块叶子也写位图（`:1134`/`:1156`）；一个脏块可对应多个
   软件 bitmap 位，整段被保守标脏，但无法据此知道块内究竟哪个基础页被写过。
- **拆分/合并错误**：`arm_smmu_split_block` 在 `handled_size != size` 时返回
  `-EFAULT`（`:3810`-`:3812`），但被 switch 层丢弃，不向 VFIO 传播。
- **并发快照**：sync/clear 期间 DMA 仍在跑，单次 sync 的位图不保证是并发 DMA 下的全局
  原子快照。

## 十二、声明

- 本文为源码静态分析，非运行时验证，不构成正确性证明。
- provenance 仅限当前仓库 `OLK-6.6` 快照，未参照或推断上游提交历史。
- 行号引用对应写作时文件内容，后续提交可能漂移。
