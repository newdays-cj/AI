# arm64 LPA2 回合到 6.6：难度与性能分析

## 一、基本信息与证据范围

本报告评估将 FEAT_LPA2（52 位物理/虚拟寻址）特性回合到 Linux 6.6 内核的工程难度、性能影响与验证范围。用户内核**仅确认为 6.6**，未确认具体派生。本地 `/home/chenjun/Git/OLK-6.6` 是本地检查样本，未确认是用户实际部署源码[^localsnap]。

**结论：完整 host stage-1 PA+VA 回合属于高侵入、高风险的架构级 backport；若再交付 KVM 高 PA/IPA 能力，范围和验证成本进一步增加。性能上主要关注 4KB 的五级遍历、建表/拆表路径以及设备高 PA 可达性，不存在可直接套用的统一性能损失百分比。** 本次未实施回合或运行基准。

证据来源：原始 host v7 cover letter（Ard Biesheuvel, Google），基于 v6.8-rc1，标题为 `[PATCH v7 00/50] arm64: Add support for LPA2 and WXN at stage 1`，同时推进 LPA2 与 WXN[^cover]；v6.8 阶段的 KVM stage2/hyp stage1 LPA2 支持清单[^kvm68]；v6.9 上游代码永久链接[^v69links]，已确认 host LPA2 特性在 v6.9 存在；本地 OLK-6.6 Kconfig 读取[^localkcfg]。

本次已定位 v8 的维护者应用清单与关键主线提交，以及数项后续修复。**未完成的是相对用户精确 6.6 revision 的最小依赖闭包、逐提交可应用性和后续修复的穷尽性审计**；下文清单不能直接作为可批量应用的交付补丁集。

## 二、上游事实（v6.8 / v6.9）

### 2.1 host v7 cover 概述

v7 系列共 50 个补丁（00/50），标题 `[PATCH v7 00/50] arm64: Add support for LPA2 and WXN at stage 1`，基于 v6.8-rc1，作者 Ard Biesheuvel（Google）[^cover]。系列拆分：约 8 个补丁重构 VA layout 选择，约 30 个补丁把早期 init 从汇编迁到 C 以实现 CPU feature discovery 的早期化，约 10 个补丁为 feature/LPA2 本身。

cover 的关键技术点是把特性探测和覆盖处理放在创建最终内核映射之前，减少已建映射上的复杂格式转换。但**初始 ID map 仍需转换**：最终启动实现通过临时映射切换 DS 并重建 ID map，不能把“早期探测”理解成“完全没有页表格式过渡”[^boot]。WXN 是独立目标，不属于本次范围。

后续 v8 为 43 项，维护者列出了对应 commit。其最后两项为 mmap 权限校验 hook 与 WXN，41/43 为默认配置变更；这些不能自动视为 LPA2 核心实现必需项。v7 的布局调整与已有前置也不能因未出现在 v8 剩余清单中就忽略[^v8]。

| 回合依赖组 | 主线锚点 / v8 清单位置 | 对 6.6 的意义 |
|---|---|---|
| 早期 C 运行环境、重定位、特性覆盖 | v8 1-18，例如 `97a6f43bb049` | 先核对目标树是否已有等价重构 |
| permanent ID map 与初始建表 | `e6128a8e523c`、`34b98e55f684`、`84b04d3e6bdb` | ID map 不再简单跟随配置 VA/层级 |
| DS 与 PA/PTE 编解码 | `db95ea787bd1`、`925a0eb48044` | 高地址位与属性位必须一致处理 |
| 五级页表及启动启用 | `a6bbf5d4d9d1`、`9684ec186f8f` | 4KB level -1、临时 ID map 与 DS 切换 |
| fixmap、KASAN、运行时折叠、ptdump、KVM walker | v8 34-39 | 同时覆盖新硬件与回退路径 |
| 放开 4KB/16KB VA52 配置 | `352b0395b505` | 最后使能，不是独立功能补丁 |

这些锚点均可从维护者应用清单追溯[^v8]。对 PA-only、保持旧 VA 的定制目标，需要另做依赖分析；上游 6.9 配置把 4KB/16KB 的 PA52 与 VA52 联系在一起，不能靠删去五级页表补丁就宣称得到安全的 PA-only 实现。

### 2.2 KVM 侧 LPA2 在 v6.8 周期

KVM stage2 与 hyp stage1 的 LPA2 支持在 v6.8 周期出现[^kvm68]。该清单列出 11 个 Ryan Roberts 提交，外加 Anshuman Khandual 的 sysreg 前置补丁；清单中**包含 TLBI 相关补丁**。**11 这个数字是当时列出的清单计数**，不能解读为"KVM LPA2 回合 6.6 的必需总数"，因为前置依赖、修复与回滚补丁不在该清单内。

### 2.3 v6.9 上游代码核验

依据 v6.9 配置及页表代码[^v69links]，基础页映射的完整硬件遍历深度如下；block/大页映射可以提前终止遍历：

| 基础页 | 运行时 VA 位数 | 页表级数 | 与常见基线的区别 |
|---|---|---|---|
| 4KB | 39 | 3 | 本地检查样本采用此配置 |
| 4KB | 48 | 4 | 48 位基线 |
| 4KB | 52 | 5 | 相对 VA48 增加硬件 level -1 |
| 16KB | 47 | 3 | 52 位构建在不支持 LPA2 时的回退配置 |
| 16KB | 48 | 4 | 可以独立选择的旧配置 |
| 16KB | 52 | 4 | 硬件 level 0-3，顶层覆盖范围扩大 |

4KB 的 52 位构建在不支持 LPA2 时回退为 VA48，通过运行时折叠 P4D 恢复四级页表；16KB 则回退为 VA47 并折叠 PUD，恢复三级页表。FEAT_LPA2 不改变描述符大小，PTE 仍为 8 字节。

### 2.4 描述符编码

依据 v6.9 `pgtable.h`、`pgtable-hwdef.h`、`pgtable-prot.h`[^v69links]：`PA[49:48]` 原生存放，`PA[51:50]` 存放在描述符 `[9:8]`（原先这两位是 shareability）；TCR.DS=1 时相关 Normal memory 的 shareability 改由 TCR 控制，不意味着 Linux 将正常共享内存改为 Non-shareable。CONT（bit 52）、GP（bit 50）、PXN（bit 53）位置**不变**。在 `CONFIG_ARM64_PA_BITS_52` build 中 `__pte_to_phys` mask 掉 `PTE_MAYBE_SHARED` 并解码高位；非 PA_BITS_52 build 中该宏更简单。`lpa2_is_enabled()` 读 TCR.DS。

**关键风险**：高 PA 位复用 shareability 字段，编码/解码正确性与 TLBI range 地址编码是必须严格审计的两条路径。CONT 位本身不冲突，但"CONT bit 52 在 LPA2 启用后语义不变"必须与软件辅助路径一起验证，不能孤立声称安全。

### 2.5 上游 Kconfig 耦合

v6.9 Kconfig 中 `CONFIG_ARM64_LPA2` 依赖 `PA_BITS_52 && !64K`，而 `PA_BITS_52` 在该版本依赖 `64K || VA52`[^v69links]。这是**上游配置耦合关系**，不是架构必然性。架构上 FEAT_LPA2 与 4K/16K 兼容（见 2.3），耦合是 Kconfig 组织选择。

## 三、改动目的

FEAT_LPA2 的核心目标是**在不切换基础页大小的情况下扩展地址宽度**：保留 4KB/16KB 部署兼容性与细粒度分配，使系统可以访问地址不低于 `2^48` 的物理资源，并提供更大的虚拟地址空间。它不要求安装超过 256 TiB 的 RAM，也不保证完整 4 PiB RAM 均可被所有内核配置直接映射。

相比改为 64KB 页，保留页大小避免了额外的部署适配及内部碎片变化。等量 RAM、相同 `struct page` 大小时，64KB 页的元数据比例反而更低；页大小与 CONT 分组大小的变化应单独评估。**LPA2 的主要收益是可寻址容量，不是固有的速度提升。**

## 四、方案描述

### 4.1 早期 init 重构为 C

依据 cover[^cover]，上游把早期建表、重定位和特性覆盖处理迁入 C，在创建内核映射之前确定硬件能力与命令行覆盖结果，从一开始选择匹配的描述符格式。沿上游实现回合时，需要核对这组早期启动重构的依赖，而不是只摘取最终启用 LPA2 的提交。它并不意味着架构上只能采用这一种软件实现。

### 4.2 4K/16K 多级页表

依据 v6.9 代码[^v69links]，4KB + VA52 使用 PGD/P4D/PUD/PMD/PTE 五级，逻辑顶层有 16 项；4KB + VA48 的四级页表顶层有 512 项。不能只比较顶层项数就判断总开销，因为五级结构还多出了一层表。16KB + VA52 使用硬件 level 0-3，与 VA48 同为四级。

按 `PGD_SIZE = PTRS_PER_PGD * sizeof(pgd_t)` 与 v6.9 `pgd_alloc()` 推导：LPA2 实际启用时，4KB 的 PGD cache 对象为 128 字节，16KB 的对象为 256 字节；需要运行时折叠时，顶层改用整页分配[^pgdalloc]。这是对象大小，不包括 slab 利用率和额外中间页表，不能据此宣称整个进程页表更省内存。

### 4.3 软件辅助路径

依据 v6.9 `pgtable.h` 与 `pgtable-prot.h`[^v69links]：在 `CONFIG_ARM64_PA_BITS_52` build 中 `__pte_to_phys` mask `PTE_MAYBE_SHARED` 并解码高 PA 位，`lpa2_is_enabled()` 运行时读 TCR.DS；非 PA_BITS_52 build 中宏更简单。即使在 fallback（硬件不支持 FEAT_LPA2）的 PA_BITS_52 build 中，软件辅助 helper 仍会被编译进内核。fallback 路径是否走快路径、辅助开销是否为零，**没有证据保证**。

## 五、关键实现细节（回合侧风险）

**编码/解码正确性**：高 PA 位复用 shareability 字段（描述符 `[9:8]`）。回合时必须审计 `set_pte`/`__set_pte_at`/`pte_mkclean` 等所有写描述符路径，确认 `[9:8]` 在 FEAT_LPA2 启用时不再写 shareability；`pte_to_phys`/`__pte_to_phys`/`pfn_pte` 等所有读 PA 路径确认高位解码；DMA/IOMMU/SWIOTLB 路径对 PA 高位的处理；kdump、kexec、KASLR 早期地址转换。

**TLBI 范围编码**：FEAT_LPA2 改变 VA 宽度，TLBI range 指令的地址编码必须重新核对。v6.8 KVM 清单[^kvm68] 中包含 TLBI 相关补丁，因此这不是"上游未交付"的事项，但回合时地址编码审计仍是强制性约束。

**DS 过渡一致性**：早期 idmap 描述符在 DS 设置前后语义不同，回合必须保证 DS 切换时序与所有 live 描述符编码匹配。不能只向旧 6.6 的 TCR 设置路径添加一个 DS 位；也不能把正确性寄托于现有低地址 RAM 恰好没有用到高 PA 位。

具体上游实现中的 `remap_idmap_for_lpa2()` 创建临时 LPA2 兼容 ID map，切换 TTBR/TCR，重建原始 ID map 后再切回。寄存器切换有短暂的 MMU 控制步骤，但不等于在 MMU/cache 全关闭环境中随意改写全部页表。该实现还明确要求初始 kernel image 位于 48 位可寻址 PA 范围；运行后能管理高 PA，不能推出启动镜像可以任意放到高 PA[^boot]。

### 已定位的后续修复

| 提交 | 已证实的问题 | 纳入条件 |
|---|---|---|
| `ecc54006f158` | 清零初始 ID map 的长度误写成 start-end | 回合相应启动重映射路径时核对等价修复 |
| `36639013b346` | 16KB 非 LPA2 回退时，动态折叠与 fast-GUP lockless walker 组合导致随机 oops | 保留 16KB 回退时重点验证 |
| `bf74bb73cd87` | 关闭 LPA2 后仍接受高 PA，产生截断和错误 shareability | 必须核对运行时 PA 上限，而非只看编译宏 |
| `f0da16992aef` | HYP TCR.PS/DS 错用 stage-2 状态，未匹配 host stage-1 | 交付 KVM 时核对 |
| `3648027de1fa` | 五级 trampoline 克隆页表时遗漏 P4D 分配，休眠恢复可崩溃 | 启用相关休眠/kexec 路径时核对；提交称 kexec 可能同样受影响 |

以上已直接核对主线补丁说明[^fixes]，是“仅合入最初功能补丁不够”的实际例证，不是完整修复清单。修复自身也可能有后续前置，需按目标代码做语义适配。

## 六、方案的缺陷与使用限制

### 6.1 性能不必然提升

FEAT_LPA2 主要收益是**容量**。新增页表级别的影响：TLB miss 路径增加 1 级 walk（4K VA52），TLB hit 路径不变但 hit access 不是硬件 walk，同样 granule/block mapping 的 TLB reach **不会自动变好**，sparse 高 VA 用户空间可能分配更多中间页表。不能给出"提速 X%"的百分比，因为没有可比基准。

### 6.2 root 表与下层表分配

4KB VA52 的顶层只有 16 个逻辑项且使用较小的 PGD cache 对象，但五级结构增加中间层。应把 PGD 对象、额外中间表、slab/page 分配粒度一起统计；不能把总增量一概写成固定的一页。进程数量、地址分布、THP/hugetlb 使用都会改变实际开销。16KB 从 VA48 到 VA52 的顶层由 2 项增至 32 项，没有增加硬件遍历层级[^pgdalloc]。

### 6.3 vmemmap 保留不等于实际分配

vmemmap 为更大的可映射 PFN 范围预留虚拟空间，但**保留虚拟地址不等于实际 `struct page` 已分配**。假设 `sizeof(struct page)=64`，常规 RAM 的元数据/数据比为 `64/PAGE_SIZE`：4KB 页约 1.5625%，16KB 页约 0.390625%。这是理论比例，实际受结构大小、空洞处理及元数据优化等影响；THP 通常也不消除每个基础页对应的 `struct page`。同样 RAM 容量、同样页大小下，不能因为地址范围扩大 16 倍就声称元数据也扩大 16 倍。

### 6.4 软件辅助开销可能存在

依据 v6.9 helper 实现[^v69links]，软件 mask 与解码在 PA_BITS_52 fallback 路径也存在。**没有证据保证 fallback 性能与未启用 LPA2 的 build 完全一致**。

### 6.5 leaf PT dense 比例（参考）

理想 dense leaf PT 比例（每页 8 字节 PTE，8/PAGE_SIZE）：4K ≈ 0.1953125%，16K ≈ 0.048828125%。**理想 dense 不含上层页表与 THP**，仅供量级参考。

### 6.6 部署与兼容性约束

- **PAC 位预算**：本地 Kconfig 对 52 位 VA 与 ARMv8.3 PAC 的组合提示 PAC 从 7 位减少至 3 位[^localkcfg]。这属于 VA 扩展的安全权衡，不是单独扩大 PA 的必然代价；具体 PAC 模式须按目标 CPU 核验。
- **用户态兼容性**：内核默认保留较低地址的分配策略，应用通过高地址 `mmap()` hint 请求更大 VA；依赖指针高位保存信息的 JIT、运行时及自定义分配器需要测试。不要在生产中用调试选项 `ARM64_FORCE_52BIT` 强迫所有分配走高地址[^vaabi]。
- **内核工具链条**：核对外部模块中的页表假设，以及 crash/makedumpfile 等工具对新布局和页表层级的支持；调试使用匹配的 `vmlinux`。这不意味着现有工具必然失败。
- **DMA/IOMMU**：CPU PA 位宽、设备 DMA mask、IOMMU 输入/输出地址位宽分别核查。不能把 CPU 页表的 LPA2 描述符格式直接套用到 SMMU。若高 PA 必须通过 SWIOTLB bounce，额外复制可能成为主要成本；有合适 IOMMU 映射时则不必然需要 bounce[^dma]。
- **KVM 地址维度**：guest IPA（stage-2 输入）和 host PA（stage-2 输出）不同。即使限制 guest IPA 不超过 48 位，guest backing RAM 也可能处于高 host PA；不能因此跳过 KVM 页表编码审计。

另需单列 **SVA**：共享进程 CPU 页表时，SMMU 必须理解实际描述符格式。上游 `6e192214c6c8` 明确在当时不支持 LPA2 的 arm-smmu-v3 路径拒绝此类 SVA[^sva]。因此应检查目标 6.6 是否有等价限制，而不是把普通独立 IOMMU 页表与 SVA 混为一谈。该历史修复不代表所有当前 SMMU 都不支持 LPA2。

## 七、OLK-6.6 回合难度与本地样本

### 7.1 本地 Kconfig 样本

对 `/home/chenjun/Git/OLK-6.6/arch/arm64/Kconfig` 的读取[^localkcfg]确认：行 1529-1541 `ARM64_VA_BITS_52` 依赖 `ARM64_64K_PAGES && (ARM64_PAN || !ARM64_SW_TTBR0_PAN)`，提示与 ARMv8.3 PAC 共存时 PAC 从 7 位降为 3 位；行 1579-1589 `ARM64_PA_BITS_52` 依赖 `ARM64_64K_PAGES`；行 1502、1516 4K 默认 VA39（`ARM64_VA_BITS_39 depends on ARM64_4K_PAGES`）；**未发现 `ARM64_LPA2` Kconfig 符号**（grep 全文件无匹配）。该样本中 52-bit PA/VA 仅通过 64K 页提供，**没有 4K/16K LPA2 路径**，与上游 v6.9 已存在的 `ARM64_LPA2` 形成结构性差异。

### 7.2 本地 .config 提示

本地 `/home/chenjun/Git/OLK-6.6/.config` 是本地实际读取的文件（行 482-489 为 4K VA39 PA48，KVM 行 793 enabled），但**未确认是用户实际运行的配置**。该提示仅说明在 4K 页 + VA39 + PA48 的常规 OLK-6.6 build 中 LPA2 缺席是预期状态。

代码缺口不止配置：该样本的 `head.S` 仍在汇编中创建初始 ID map，`proc.S` 的 TCR 设置尚无 LPA2 DS 启用路径；`pgtable-types.h` 与 KVM 页表帮助函数也仍带有旧层级/64KB PA52 的假设[^localpaths]。这些是集成风险的代码证据，但未经实际应用补丁，不能据此声称已经统计出文本冲突数量。

### 7.3 回合难度排序（工程估计）

本节为**经验性估计**，在依赖闭包未完成前不作为承诺：

| 范围 | 难度 | 说明 |
|------|------|------|
| host FEAT_LPA2（4K/16K VA+PA） | 高 | 早期 init C 化、DS 过渡一致性、描述符编码、TLBI |
| 仅 KVM stage-2 + hyp stage-1 | 中高，独立范围 | 相对集中，但不交付 host stage-1 LPA2 |
| host + KVM 高 PA/IPA | 更高 | 两套页表、host/hyp 一致性、VHE/nVHE 与迁移 ABI |

以上为工程判断，不是上游给出的工期。当前缺少精确 revision、目标页大小、硬件与产品功能清单，不给可承诺的周数。限定单一页大小、关闭 KVM/休眠等功能的原型会明显收敛；覆盖 4KB、16KB、老 CPU 回退、KVM 和生命周期路径的产品版本需要多轮集成与回归。向 guest 暴露完整 LPA2 特性及迁移兼容性应另列验收，不能与“stage-2 可编码高 PA”画等号。

### 7.4 未确认边界

相对目标 6.6 的最小补丁集仍未闭包：v7 的 50 项、v8 的 43 项和 v6.8 KVM 清单计数都不能直接相加作为交付量。本次**未做应用补丁试验、编译验证、QEMU 启动或 benchmark**；已定位主线锚点，不代表已证明在用户树上可无冲突应用。

## 八、性能预估与基准测试建议

### 8.1 控制变量

比较 FEAT_LPA2 启用前后的性能必须固定：THP、hugetlb、CONT 策略、NUMA 放置、PAC、KASAN、CPU 频率与调度策略，以及实际内存容量和工作集。**不要通过切换页大小来解释 LPA2 回归**：4KB、16KB、64KB 是独立变量。

### 8.2 必须分组的对照

区分 **build VA/max** 与 **actual VA**（运行时实际生效的 VA 宽度）：

| 组 | 基础页 | build VA | actual VA | 备注 |
|----|--------|----------|-----------|------|
| A | 4K | 39 | 39 | 本地 OLK-6.6 样本（可选对照） |
| B | 4K | 48 | 48 | 原始 6.6 VA48 与 patched VA48 对照 |
| C | 4K | 52 | 48 | patched 52 build，硬件不支持时回落到 actual 48 |
| D | 4K | 52 | 52 | patched 52 build，硬件支持时 actual 52 |

优先在同一硬件上比较原始 6.6 VA48 与回合后 VA48，隔离启动重构等代码改动的影响。C、D 可以使用同一份 52 位构建，但若要在同一 CPU 上比较两种模式，必须先确认回合版本提供并正确处理启动时特性覆盖；不能直接在运行中翻转 DS。若 C、D 只能在不同 CPU 上测试，则差异混入硬件因素，不足以直接归因于 LPA2。

16K 基线应单独分组：47-bit/3 级 vs 48-bit/4 级 vs 52-bit/4 级分别对照。

### 8.3 测量点建议

建议分别测量：跨越 TLB/cache 容量阈值的随机 pointer-chase；`fork/mmap/mprotect/munmap` 与缺页成本；目标业务吞吐和 p95/p99 延迟；`VmPTE`、`/proc/meminfo` 中的页表开销。统计口径需核对，`VmPTE` 不能直接等同完整的内核映射、KVM 与所有元数据成本。CPU PMU 事件按目标实现选择，用 walk 次数/周期等指标解释结果，不能跨 CPU 生搬事件号。**QEMU/FVP 用于功能验证，不能代替真实 CPU 的 TLB、walk cache 和内存延迟测量。**

性能推断须区分两条路径：TLB 命中时不进行完整硬件页表遍历；未命中时，多出的上层项可能已被页表遍历缓存或普通 cache 缓存，因此四级变五级不等于业务延迟增加 25%。相反，TLB 压力大、随机访问和频繁建表的负载更需要重点实测。**4KB 实际运行 VA52 时，即使应用只映射低 VA，也不能因未使用高地址而免去第五级遍历；低地址默认分配策略是 ABI 兼容机制，不是自动切回四级页表。** 相同基础页大小和映射粒度下，LPA2 不会自动扩大 TLB reach。

本次核查的 host LPA2 系列没有提供可据以填写“启用后下降多少”的受控 A/B 基准。16KB 回退到三级的性能动机是上游设计说明，不是性能实测。依赖补丁中即使存在独立 TLBI 优化数据，也不能归因于 LPA2。若包含 KVM，应另测 guest 的两阶段转换、dirty logging 和 memslot 操作，不能把 host walk 变化直接换算成 guest 业务损失。

### 8.4 高 PA 测试方法

区分 **2^48 地址边界**与**实际安装 RAM 容量**：实际高 PA 测试**不需要填充 256 TiB RAM**，用 sparse RAM above 2^48 模拟即可，内存模型、地址映射、boot 内存放置必须支持高 PA。高 userspace VA hint 仅验证 VA，**不验证 PA**。

## 九、验证范围与不确定性

### 9.1 本报告已核验

本地 OLK-6.6 Kconfig 1529-1541 与 1579-1589 行[^localkcfg]；本地 Kconfig 无 `ARM64_LPA2` 符号（grep 全文件）[^localkcfg]；v6.9 上游代码永久链接读取结论（见 2.3、2.4）[^v69links]，确认 host FEAT_LPA2 特性在 v6.9 存在；v7 cover 的系列拆分与早期 probe 说明[^cover]；v6.8 KVM LPA2 清单的存在及 TLBI 补丁在场[^kvm68]。

### 9.2 本报告未核验

相对用户精确 revision 的完整依赖闭包、所有后续修复及其适用性；OLK-6.6 与上游 v6.6 的逐提交差异和局部厂商实现；用户实际部署的配置、CPU/SMMU/固件能力；任何应用补丁、编译、启动或性能测试结果。当前局部代码检查不能替代完整历史与依赖审计。

### 9.3 建议的完整验证清单（如进入回合）

如果进入实际回合，建议覆盖：boot、fallback、mixed CPUs、THP、TLBI、KASAN、CPU hotplug、kexec、kdump、KVM VHE/nVHE（如 scope）、DMA/SWIOTLB。

重点验收应包括：高 VA `mmap()` 与高 PA 映射分别证明；关闭 LPA2 时拒绝或裁剪不可表达的高 PA；高地址内存热插拔与 vmemmap 在线/离线；16KB 回退时 fast-GUP 压力；4KB 五级时 kexec/休眠；KVM 高 host PA、P52 guest modes、dirty logging 和 memslot 增删。混合能力 CPU 必须按系统能力策略正确接纳或拒绝。QEMU 的具体版本、CPU 模型和 LPA2 属性应先确认，不能假定任意版本的 `-cpu max` 都能覆盖上述路径。

## 十、推荐

1. **完成依赖闭包与 fix 审计**：v7/v6.8/v6.9 系列的精确最终依赖闭包、commit 映射与后续修复完整性
2. **不要在依赖未闭包前手写大块最小集**：v7 cover 的 50 补丁含 WXN，不能作为最小集起点
3. **不要批量导入无关 WXN**：WXN 是独立目标，回合 LPA2 不应顺带引入
4. **完整 host 回合定为高难度，host + KVM 产品化范围更大，避免过早承诺工期**
5. **本地 OLK-6.6 样本未确认是用户实际部署源码**，不能替代用户实际基线核验
6. **把功能与性能分别验收**：能编译、能启动或能分配高 VA，均不足以证明高 PA、TLBI 和真实负载性能已通过验证

## 参考来源

[^cover]: Ard Biesheuvel (Google), "[PATCH v7 00/50] arm64: Add support for LPA2 and WXN at stage 1" (cover letter), 2024-01-23, Message-ID `20240123145258.1462979-52-ardb+git@google.com`. LWN 镜像 <https://lwn.net/Articles/959155/>

[^v8]: Catalin Marinas，v8 系列应用回信及逐提交清单，2024-02-16：<https://lists.infradead.org/pipermail/linux-arm-kernel/2024-February/904912.html>。原始系列：[lore](https://lore.kernel.org/r/20240214122845.2033971-45-ardb+git@google.com/)。

[^boot]: [9684ec186f8f: Enable LPA2 at boot if supported by the system](https://github.com/torvalds/linux/commit/9684ec186f8fadde52d6b6eaf64ca508897d0c71)。

[^fixes]: 主线修复：[初始 ID map 清零](https://github.com/torvalds/linux/commit/ecc54006f158ae0245a13e59026da2f0239c1b86)、[lockless folding/GUP](https://github.com/torvalds/linux/commit/36639013b3462c06ff8e3400a427f775b4fc97f5)、[运行时 PA 范围](https://github.com/torvalds/linux/commit/bf74bb73cd87c64bd5afc1fd4b749029997b6170)、[HYP TCR.PS/DS](https://github.com/torvalds/linux/commit/f0da16992aef7e246b2f3bba1492e3a52c38ca0e)、[kexec/hibernate trampoline](https://github.com/torvalds/linux/commit/3648027de1fa91a0c80cffd3ecff263d06e62605)。

[^sva]: [6e192214c6c8: Document SVA interaction with new pagetable features](https://github.com/torvalds/linux/commit/6e192214c6c82c2f52238d5e1865f11594e58a6f)。

[^pgdalloc]: Linux v6.9 [pgd.c](https://github.com/torvalds/linux/blob/v6.9/arch/arm64/mm/pgd.c) 的 `pgdir_is_page_size()` / `pgd_alloc()` / `pgtable_cache_init()`，以及 [pgalloc.h](https://github.com/torvalds/linux/blob/v6.9/arch/arm64/include/asm/pgalloc.h) 的 `PGD_SIZE` 与 `p4d_alloc_one()`。

[^kvm68]: Ryan Roberts et al., arm64 KVM stage2/hyp stage1 LPA2 系列（v6.8 周期），含 TLBI 相关补丁, 2024-01. 列出 11 个 Ryan Roberts 提交加 Anshuman Khandual sysreg 前置. <https://lists.infradead.org/pipermail/linux-arm-kernel/2024-January/893347.html>

[^v69links]: Linux v6.9：[Kconfig](https://github.com/torvalds/linux/blob/v6.9/arch/arm64/Kconfig)、[pgtable.h](https://github.com/torvalds/linux/blob/v6.9/arch/arm64/include/asm/pgtable.h)、[pgtable-hwdef.h](https://github.com/torvalds/linux/blob/v6.9/arch/arm64/include/asm/pgtable-hwdef.h)、[pgtable-prot.h](https://github.com/torvalds/linux/blob/v6.9/arch/arm64/include/asm/pgtable-prot.h)、[memory.h](https://github.com/torvalds/linux/blob/v6.9/arch/arm64/include/asm/memory.h)。

[^vaabi]: Linux v6.9 [ARM64 memory.rst](https://github.com/torvalds/linux/blob/v6.9/Documentation/arch/arm64/memory.rst)，52-bit userspace VAs；本地 Kconfig 的 `ARM64_FORCE_52BIT` 帮助文本也限定其用于测试。

[^dma]: Linux v6.6 [DMA API HOWTO](https://github.com/torvalds/linux/blob/v6.6/Documentation/core-api/dma-api-howto.rst)。此处为按 DMA 地址契约提出的部署核查项，并非声称需要修改所有 DMA/IOMMU 驱动。

[^localsnap]: 本地 `/home/chenjun/Git/OLK-6.6` 是本地检查样本，未确认是用户实际部署源码。用户内核仅确认为 6.6。

[^localkcfg]: `/home/chenjun/Git/OLK-6.6/arch/arm64/Kconfig` 读取：行 1529-1541（`ARM64_VA_BITS_52` 依赖 `ARM64_64K_PAGES`，PAC 7→3 提示），行 1579-1589（`ARM64_PA_BITS_52` 依赖 `ARM64_64K_PAGES`），行 1502/1516（4K 默认 VA39）。全文件 grep `ARM64_LPA2|LPA2` 无匹配。

[^localpaths]: 本地样本：`arch/arm64/kernel/head.S:334`（初始 ID map）、`arch/arm64/mm/proc.S:437`（TCR 设置）、`arch/arm64/include/asm/pgtable-types.h:27`（层级类型）、`arch/arm64/include/asm/kvm_pgtable.h:61`（PA 编解码）。该树还含 RME/TMI 协议中的 LPA2 字样，不代表通用 host/KVM 页表路径已支持 LPA2。
