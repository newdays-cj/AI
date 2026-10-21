# ARM SMMU contiguous hint（CONT）bit 社区特性分析

## 一、基本信息与结论

分析日期：2026-10-17。用户所说的“continue bit”，本文按检索到的社区特性 **contiguous hint bit（CONT，连续提示位）**理解。分析对象是 ARM LPAE 转换表叶描述符中的 bit 52，不是 SMMUv3 STE/CD 中的同名提示字段。[^v5]

主分析补丁为 `[PATCH v5] iommu/io-pgtable-arm: Add support for contiguous hint bit`，由 Vijayanand Jitta 于 2026-09-21 发出，补丁正文作者为 Prakash Gupta。修改 `drivers/iommu/io-pgtable-arm.c` 和 `include/linux/io-pgtable.h`，115 行新增、13 行删除。[^v5]

**核心结论：这是通过连续组约束改善 SMMU TLB 覆盖范围的优化，不是新增地址转换能力，也不是把多个页表项真正合成一个大页描述符。** 难点不是设置 CONT，而是保证解除映射、TLB 失效和脏页追踪始终遵守连续组语义。

本次检索到的最新修订为 v5，后续评审读至 2026-10-09；未检索到更晚修订不等于证明不存在。主线核查点为 Torvalds/linux 提交 `58161ead29e50d70a317ea26b472e0c7508e69da`（2026-10-17）：该提交的 `io-pgtable-arm.c` 没有 `ARM_LPAE_PTE_CONT`，映射叶路径仍仅判断 `size == block_size`，也没有 CONT 尺寸扩展。因此 **该 io-pgtable-arm 支持尚未进入本次核查的主线版本**。[^mainline][^master]

这一结论不代表“SMMU 不会遇到 CONT”：SVA 共享 CPU 页表时，SMMU 已可能读取由 CPU 内存管理代码建立的 CONT 映射，属于另一条路径。

## 二、改动目的与收益

### 2.1 为什么需要 CONT

设备 DMA 使用大量小粒度 IOVA 映射时，有限的 SMMU TLB 容量可能成为瓶颈。传统 block 映射可以扩大 TLB 覆盖范围，但要求按 block 大小连续和对齐。例如在 4 KiB granule 下，普通 L2 block 是 2 MiB；一段 64 KiB 连续内存达不到这个大小，通常仍需 16 个 4 KiB 描述符。[^v5]

CONT 允许一组相邻、属性一致、输出地址连续且自然对齐的描述符共同提供连续性提示。硬件可以据此推断组内其他描述符的性质，减少相关读取，并可能用更少的 TLB 表项缓存整组翻译。[^review2025]

### 2.2 一个具体例子

4 KiB granule 下，16 个连续的 4 KiB 页组成 64 KiB CONT 组：

```text
IOVA: 0x10000 ... 0x1ffff    输出地址: 0x20000 ... 0x2ffff
16 个 4 KiB 描述符，属性一致，每项 CONT=1
硬件可能将这一组缓存为覆盖 64 KiB 的翻译
```

这是**单条翻译覆盖范围的潜在扩大**，不等于吞吐提高 16 倍。页表中仍有 16 个描述符，不会因此减少对应叶表项数量；CONT 是 hint，不能假定硬件一定以某种方式合并。实际收益取决于硬件实现、TLB miss 比例、访问局部性及满足连续组条件的映射比例。

本次读到的补丁与讨论没有提供吞吐、延迟、TLB miss 或提升百分比的量化 benchmark。2026 年初版作者报告过 QEMU arm64/SMMUv3 的映射与解除映射测试及页表检查，但不能将它视为真实硬件的性能验证，也不能直接替代 v5 验证。[^v1]

## 三、硬件语义与适用条件

以下组大小来自 v5 补丁的注释和 `arm_lpae_num_cont()`，不是脱离 granule/页表层级的任意组合。[^v5]

| 页表 granule | 页级 CONT 组 | block 级 CONT 组 | L1 block 级 CONT 组 |
|---|---|---|---|
| 4 KiB | 16 × 4 KiB = 64 KiB | 16 × 2 MiB = 32 MiB | 16 × 1 GiB = 16 GiB |
| 16 KiB | 128 × 16 KiB = 2 MiB | 32 × 32 MiB = 1 GiB | 本补丁未提供 |
| 64 KiB | 32 × 64 KiB = 2 MiB | 32 × 512 MiB = 16 GiB | 本补丁未提供 |

连续组成立需要满足以下条件：[^review2025][^v5]

- 输入地址范围连续，组起点按整个组的大小自然对齐。
- 输出地址范围连续，输出起点也按整个组的大小对齐。
- 组内描述符处于相应层级，具有要求一致的权限与内存属性。
- 按要求对整组设置 CONT，并在修改、撤销和失效过程中保持组语义。

普通 DMA 页表的输出通常是物理地址；若分析涉及嵌套翻译，应按具体转换阶段理解输出地址，不能笼统假定每一级输出都是最终 PA。仅 IOVA 连续、后端物理页零散的 scatter-gather 映射不能因此使用 CONT。

CONT 与 hugetlb/THP 的目的有相似处，但不是同一机制。该补丁不修改 CPU 页表管理，也不要求应用使用 hugetlb；I/O 页表 granule 也不能简单等同于所有场景下的 CPU `PAGE_SIZE`。

## 四、v5 软件方案

### 4.1 用 `pgsize_bitmap` 表达连续组能力

`arm_lpae_restrict_pgsizes()` 先保留当前 granule 合法的普通页/block 大小，再在未设置禁用 quirk 时派生 CONT 大小。例如 4 KiB granule 可在原有 4 KiB、2 MiB、1 GiB 之外，增加 64 KiB、32 MiB、16 GiB 候选。[^v5]

这些新大小是对 IOMMU API 暴露的有效映射粒度，不代表硬件增加了对应大小的单个页表描述符。IOMMU core 可以通过常规页大小选择路径选择 CONT 粒度；io-pgtable 再将一个 CONT 粒度转换成多个普通描述符。

v5 去除了内部 prefix/group/suffix 扫描提升。对齐情况下，`map_pages(pgsize=4K, pgcount=16)` 不会自动变成 CONT；`map_pages(pgsize=64K, pgcount=1)` 才进入对应路径。相邻的多次小粒度映射也不会在事后自动重新组合。[^v5][^direct]

### 4.2 映射路径与字节记账

补丁新增：[^v5]

```c
#define ARM_LPAE_PTE_CONT (((arm_lpae_iopte)1) << 52)
```

`__arm_lpae_map()` 在当前层级计算普通 `block_size`、组内描述符数 `num_cont` 和 `cont_size`。它接受普通尺寸，或在功能启用时接受恰好等于 `cont_size` 的请求。

CONT 请求显式检查 IOVA 与输出地址对齐。随后计算：

```c
entries_per_map = size / block_size;
num_entries = min_t(size_t, pgcount,
                    max_entries / entries_per_map) * entries_per_map;
/* CONT 请求将 ARM_LPAE_PTE_CONT 加入 prot */
```

由 `arm_lpae_init_pte()` 安装这些描述符。统一的 `prot` 和顺序递增的输出地址提供组内属性一致及输出连续性；这不是扫描、改造已经存在的任意映射。

关键记账是 `*mapped += num_entries * block_size`。`num_entries` 已是实际描述符数量，不能再乘原始 CONT 尺寸，否则会重复放大。页表项初始化和非一致性 walker 的同步仍沿用现有辅助路径；该补丁没有引入一套通用的在线连续组升级协议。[^v5][^mainline]

### 4.3 解除映射：不支持任意拆组

`__arm_lpae_unmap()` 同样区分 API 请求尺寸与实际描述符尺寸。在修改当前批次描述符前，检查首尾实际 PTE 的 CONT 位，以及请求起点、终点是否落在组边界。若会切开已标记组，则 `WARN_ONCE()` 并返回 0，不清除该批次任何描述符。[^v5]

这意味着对一个 64 KiB CONT 组，仅解除其中 4 KiB 的请求会被拒绝，而不是自动拆组。没有 CONT 标记的普通项不能仅因为请求大小数值相同，就被误判为 CONT。

“拒绝时不修改”应限定为该检查对应的内部处理批次，不是整个上层 `iommu_unmap()` 请求的事务回滚保证；不能据此假设此前成功解除的其他范围会恢复。这是对接口语义的分析，不是作者提供的事务测试结果。

### 4.4 配置、格式和地址位宽

v5 使用 `IO_PGTABLE_QUIRK_ARM_NO_CONT_HINT` 按页表实例禁用，不再使用 v1 的独立 Kconfig。它是驱动初始化配置，不是用户可随时切换的 sysfs 开关。`ARM_MALI_LPAE` 内部设置禁用 quirk，因为其格式没有该 CONT 位。[^v5]

补丁在计算组大小时检查 `ULONG_MAX` / `SIZE_MAX`，避免 32 位乘法溢出；最终按 IAS/OAS 裁剪 `pgsize_bitmap`，保留大小恰好等于整个地址空间的候选，因为地址零仍可容纳它。注意这些是 v5 实现，部分处理方式仍被评审质疑，不能当成社区已确认的最终设计。[^v5][^robin-code]

## 五、社区演进与关键争议

### 5.1 从隐式发现连续组转向显式尺寸

Daniel Mentz 于 2025-04-30 发布 `Support contiguous bit in translation tables`：对单次 `map_pages()` 完整覆盖的组置 CONT。Jason 和 Robin 指出需要通过 `pgsize_bitmap` 告知上层、保护部分 unmap，并提供硬件选择机制。[^initial2025][^review2025]

2026 年 Qualcomm 系列的 v1 已通告 CONT 尺寸，但使用 Kconfig，且在解除映射前清除 CONT。Jason 明确指出：**仅清 CONT 不等于 Break-Before-Make（BBM）**；BBM 的关键是使旧映射无效并完成相应失效，而不是单纯修改提示位。[^v1][^bbm]

v2 改成运行时 quirk，增加 4 KiB granule 的 L1 CONT，并拒绝拆组；v3/v4 修正对齐、格式、计数与解除映射处理；v5 移除内部自动分组，改成调用方明确选择 CONT 尺寸，并提前执行边界检查。[^v5]

### 5.2 失效 erratum：整组 unmap 不自动等于单条安全 RIL

相关 SMMUv3 补丁说明列出 MMU-700 erratum **3777127** 和 MMU S3 erratum **3673557**：CONT 翻译可能失效不完整，推荐使用完整覆盖组的范围失效（Range Invalidate，RIL）。如果两条 RIL 的分割点落在组内，即使总范围覆盖整组，也不足以规避问题。[^sva-body]

7 月 Daniel 认为拒绝部分 unmap 可隐含满足 workaround；9 月 Jason 要求 SMMUv3 处理或禁用该功能。**但讨论没有止于 blanket disable**：[^erratum][^jason-sep]

- 10 月 9 日 Robin 指出，IOMMU API 与 SVA 不同：合法的 I/O unmap 应按 CONT 粒度选尺寸，失效拆分应发生在组边界。[^robin-ril]
- Jason 随后澄清：如果 gather 只有单一页尺寸且起点对齐，拆分算法可能安全；他没有完成对该系列的完整分析，要求补充安全性解释，原意是“仅使用 RIL 不够”。[^jason-oct]

因此待验证的不变量是 **gather 的起点、粒度、合并和 flush 规则能否保留 CONT 边界**。尤其 v5 仍以 `block_size` 而不是原始 CONT 大小调用 `io_pgtable_tlb_add_page()`，不能仅凭 `pgsize_bitmap` 或软件整组清除就宣布失效路径已被证明安全（基于代码和讨论的分析）。[^v5][^jason-oct]

### 5.3 SVA 是相关但独立的问题

SVA 共享 CPU 进程页表，CONT 可由 CPU MM 建立；MMU notifier 的失效范围及驱动拆分不能直接套用 I/O 页表固定粒度假设。Jason 的另一个系列中，v6 1/9 专门处理这一问题。[^sva-series][^sva-body]

该提案对受影响硬件的 SVA 使用单条 RIL 覆盖一次 notifier 请求，必要时过度失效；无法编码或范围过大时采用更大范围的整体失效。受影响硬件的 SVA 还要求支持 RANGE_INV。它明确要求未来 IOMMU domain CONT 支持使用合适的失效算法或禁用 CONT。这里描述的是已读取的补丁，不断言其修复已合入主线。[^sva-body]

### 5.4 硬件脏页追踪冲突

后续评审指出：启用 `IO_PGTABLE_QUIRK_ARM_HD` 后，脏页读取/清除路径按单个 PTE 修改权限状态。若 64 KiB CONT 组中的一个 4 KiB 项被标 clean、其他项仍 dirty/writeable，组内 AP[2] 就可能不一致，违背 CONT 权限一致性要求。[^dirty]

Jason 建议简化实现先让硬件脏页追踪与 CONT 互斥；作者 10 月 9 日同意在 HD 场景设置禁用 quirk。**这是后续修改承诺，不是已发布 v5 包含的代码。** 此问题说明 CONT 的维护不只涉及 map/unmap，还要覆盖其他描述符修改路径。[^dirty]

### 5.5 opt-in、复杂度与长期方向

Robin 10 月 9 日仍要求使用正向 opt-in，而非默认开启、靠 NO_CONT_HINT 退出；理由包括直接 io-pgtable 用户及非标准硬件对新增尺寸的接受程度。不能将“Arm 架构定义 CONT”推导成“所有该分配器用户都可以无条件开启”。[^robin-code]

同一评审还质疑额外除法、重复对齐检查、复杂条目计数及 IAS/OAS 无条件裁剪：最后一项会改变未启用 CONT 用户的行为，已经超出单纯加提示位。[^robin-code]

长期方向也有争议：Jason 主张推进 SMMUv3 的 iommupt 转换；Will 不希望长期维护两份架构页表实现，但特殊格式、quirk 和其他使用者仍需处理。这是维护方向讨论，不是 iommupt 已完成替代的证明。[^direction]

## 六、使用限制与落地建议

- **只适合可形成连续组的映射。** 碎片化、未对齐或属性不同的范围不能凭设置 CONT 获益。
- **收益取决于实际尺寸选择。** 直接调用 `io_pgtable_ops`、只选择普通页/block 大小的使用者不会自动受益；Panthor/Panfrost 跟进被留给后续工作。[^direct]
- **局部生命周期受限。** v5 不实现任意部分拆组、重新保护或在线升级；使用者必须理解已建立映射的粒度。
- **不能仅摘取置位代码。** 应同时审查失效 gather、RIL 拆分、硬件 revision/errata、脏页追踪及格式选择。
- **不能把 quirk 当成已接好的 workaround。** v5 仅新增通用配置和 Mali 退出，没有包含 SMMUv3 设置该退出项或 HD 互斥的后续修改；但是否需全面禁用 SMMUv3，后续评审也未形成这里可证实的最终结论。
- **没有实测性能保证。** 应在目标硬件比较 CONT 开关前后的 SMMU PMU 指标、DMA 吞吐和延迟，并记录实际 CONT 覆盖比例；QEMU 页表检查不能证明硅实现的 TLB 收益或 erratum 安全性。

若考虑 OLK-6.6 回移植，应先核对目标树的 IOMMU API、页表实现及失效路径差异，再形成完整依赖与验证方案；不能将这个尚有评审问题的 v5 当作已成熟的单补丁直接合入。本次未读取 OLK-6.6 目标源码，未开展回移植或运行测试。

## 七、分析说明

本文依据公开补丁 diff、实际邮件讨论和固定主线源码交叉分析，区分作者测试陈述、评审意见及代码推论。lore 的部分内容通过 Openwall、LKML 镜像和 Syzbot CI 补丁正文读取；永久链接与可访问镜像列在下方。

已核对 v5、10 月 9 日主要评审、SVA v6 1/9 正文及上述主线文件；未验证其他维护分支或厂商树，未穷举所有邮件、硬件 revision 和调用者。没有进行编译、QEMU 或真实硬件验证；未修改内核代码，未创建提交。

## 参考来源

[^v5]: Vijayanand Jitta / Prakash Gupta，2026-09-21，[v5 正文及 diff](https://lists.openwall.net/linux-kernel/2026/09/21/1394)；[lore 永久链接](https://lore.kernel.org/all/20260921-iommu_contig_hint-v5-1-e7fbd1c3774d@oss.qualcomm.com/)。
[^mainline]: [Torvalds/linux 固定提交中的 io-pgtable-arm.c](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/iommu/io-pgtable-arm.c)。
[^master]: [核查 master 引用的 API](https://api.github.com/repos/torvalds/linux/git/ref/heads/master)；[固定提交对象及时间](https://api.github.com/repos/torvalds/linux/commits/58161ead29e50d70a317ea26b472e0c7508e69da)。引用会移动，源码判断以固定 hash 为准。
[^initial2025]: Daniel Mentz，2025-04-30，[Support contiguous bit in translation tables](https://patchew.org/linux/20250430231924.1481493-1-danielmentz@google.com/)。
[^review2025]: Robin Murphy，2025-05-15，[包含原补丁语义及 Jason/Will 意见的回复](https://lists.openwall.net/linux-kernel/2025/05/15/1259)。
[^v1]: Vijayanand Jitta / Prakash Gupta，2026-06-18，[初版及测试陈述](https://lists.openwall.net/linux-kernel/2026/06/18/529)。
[^bbm]: Jason Gunthorpe，2026-07-03，[BBM 与 iommupt 意见](https://lists.openwall.net/linux-kernel/2026/07/03/1930)。
[^erratum]: Daniel Mentz，2026-07-07，[MMU-700 erratum 及 workaround 讨论](https://lists.openwall.net/linux-kernel/2026/07/07/2847)。
[^direct]: Vijayanand Jitta，2026-09-09，[移除隐式分组及直接 io_pgtable_ops 用户讨论](https://lists.openwall.net/linux-kernel/2026/09/09/316)。
[^jason-sep]: Jason Gunthorpe，2026-09-23，[要求处理 SMMUv3 erratum](https://lkml.iu.edu/2609.2/22056.html)。
[^robin-ril]: Robin Murphy，2026-10-09，[IOMMU API 与 SVA 粒度差异](https://lkml.iu.edu/2610.1/03773.html)。
[^jason-oct]: Jason Gunthorpe，2026-10-09，[gather 不变量及对先前意见的澄清](https://lkml.iu.edu/2610.1/03938.html)。
[^dirty]: Vijayanand Jitta，2026-10-09，[引用 Daniel/Jason 的脏页追踪问题并同意互斥](https://lkml.iu.edu/2610.1/02976.html)。
[^robin-code]: Robin Murphy，2026-10-09，[v5 代码与正向 opt-in 评审](https://lkml.iu.edu/2610.1/04288.html)。
[^sva-series]: Jason Gunthorpe，2026-09-17，[SMMUv3 invalidation v6 系列索引](https://ci.syzbot.org/series/c2e3e777-4517-4548-bdea-517fef4b18e9)。
[^sva-body]: Jason Gunthorpe，[v6 1/9 CONT under-invalidation with SVA 正文及 diff](https://ci.syzbot.org/patches/aabcb193-01af-4449-9f4b-556dd94455d9)。
[^direction]: Will Deacon，2026-07-22，[iommupt 与 io-pgtable 维护方向讨论](https://lists.openwall.net/linux-kernel/2026/07/22/2517)。
