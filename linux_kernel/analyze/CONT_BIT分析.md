# CONT 位与脏位处理分析

> 文档面向 OLK-6.6（基于 Linux 6.6 LTS 的下游分支）的 arm64 页表实现，梳理
> CONTiguous PTE（CONT_PTE）分组机制、硬件/软件脏位语义、CPU 侧各操作路径，
> 以及 CPU 与 SMMU 在脏位追踪上的差异。所有本地行号均对应本仓库当前代码。
> 源码根目录：`/home/chenjun/Git/OLK-6.6`。本文为源码与资料分析，未进行硬件实测。

---

## 1. 概述与范围

本文只做归档性技术分析，不改任何内核源码或配置。文中标注三类事实来源：

- **【架构事实】**：来自 ARMv8-A / AArch64 架构手册（ARM ARM）的硬性规定，硬件必须遵守。
- **【Linux 选择】**：Linux arm64 实现自行做出的设计决策，其他 OS 可以不同。
- **【未规定行为】**：架构手册未明确约束，不同实现可能不同，软件不能依赖单一结果。

涉及的核心本地源文件：

| 文件 | 关键符号 / 行号 |
|---|---|
| `arch/arm64/mm/contpte.c` | `contpte_convert`(49), `__contpte_try_fold`(208), `__contpte_try_unfold`(272), `contpte_ptep_get`(287), `contpte_ptep_get_lockless`(344), `contpte_wrprotect_ptes`(532), `contpte_clear_young_dirty_ptes`(550), `contpte_ptep_set_access_flags`(579) |
| `arch/arm64/include/asm/pgtable.h` | `pte_hw_dirty/pte_sw_dirty/pte_dirty`(119), `pte_mkclean`(193), `pte_mkdirty`(201), `pte_wrprotect`(211), `__ptep_get_and_clear`(1006), `__clear_young_dirty_pte(s)`(1093), `ptep_get` 等封装(1368) |
| `arch/arm64/include/asm/pgtable-prot.h` | `PTE_WRITE`(16, 别名 DBM), `PTE_DIRTY`(18, bit55) |
| `arch/arm64/mm/fault.c` | `__ptep_set_access_flags`(242) |
| `drivers/iommu/io-pgtable-arm.c` | `iopte_writeable_dirty`(170), `iopte_set_writeable_clean`(173), `io_pgtable_visit_dirty`(1325, 1335) |

---

## 2. CONT 分组的要求与 16x4K 示例

### 2.1 分组大小不是固定值

`CONT_PTES` 取决于页大小配置（`arch/arm64/Kconfig:302`，`arch/arm64/include/asm/pgtable-hwdef.h:77`）：

| 页大小 | `ARM64_CONT_PTE_SHIFT` | `CONT_PTES` | `CONT_PTE_SIZE` |
|---|---|---|---|
| 4K | 4 | 16 | 64K |
| 16K | 7 | 128 | 2M |
| 64K | 5 | 32 | 2M |

下文取 4K 页、`CONT_PTES=16`、组大小 64K；`contpte.c` 的部分注释另用 N=4 简化画图。
**这只是一个便于讲解的示例，不是通用分组数。** 在 16K 或 64K 配置下分组内的
PTE 数量完全不同，结论的形态一致但数值不同。

### 2.2 折叠前提：同一 folio

**【架构事实】** CONT 是合并缓存翻译的提示，不是把内存页表项合并为一个项。
组的输入地址和输出物理地址须满足对应范围的对齐、连续要求，所有成员须正确设置
CONT，并具有一致的属性与权限；硬件 AF/dirty 更新导致的相关位差异有专门的例外。
硬件可以合并 TLB 项，但不保证一定合并。同一 folio 则是下面的 Linux 软件前提，
不是 CONT 架构本身对内存分配器的要求。

`__contpte_try_fold`（`contpte.c:208`）在决定能否折叠前会做严格检查：

1. 虚拟地址与物理地址所确定的组边界满足 `CONT_PTE_SIZE` 对齐；单项更新路径通常
   在组末项被修改后才尝试折叠，传入地址本身不必是组首。
2. 整个 CONT 范围必须由**同一个 folio** 覆盖（`contpte.c:250`）。
3. 范围内每个 PTE 去除 dirty/young 后的 prot 与 pfn 都须逐一对齐匹配。

只要任一条件不满足就直接 `return`，不折叠。这个"同一 folio"前提是后续多处
"可以整组操作"论证的基础。special mapping 永不折叠，因为它没有 folio 背书。

### 2.3 折叠时 dirty/young 的逻辑 OR 是合成的

注释（`contpte.c:216`）与实现都强调：折叠时不关心组内各 PTE 的 access/dirty 状态，
折叠后的整组逻辑 dirty/young 取组内对应状态的 OR，重建时传播到每个成员。

**【Linux 选择】** 折叠会重新写出全部 PTE，不保留逐页 dirty 历史；普通读取则只在
返回副本中合成 OR，不回写到各 PTE。两种操作都使用汇总状态，但副作用不同。
后面第 4 节分别展开。

---

## 3. 硬件脏位 vs 软件脏位

### 3.1 Stage-1 的脏位编码

`pgtable-prot.h:16` 明确 `#define PTE_WRITE (PTE_DBM)`，即 `PTE_WRITE` 就是 DBM
（bit 51）。`PTE_DIRTY` 是软件位（bit 55，`pgtable-prot.h:18`）。

脏位判定三件套（`pgtable.h:119`）：

```c
#define pte_hw_dirty(pte)  (pte_write(pte) && !pte_rdonly(pte))
#define pte_sw_dirty(pte)  (!!(pte_val(pte) & PTE_DIRTY))
#define pte_dirty(pte)     (pte_sw_dirty(pte) || pte_hw_dirty(pte))
```

即 `pte_dirty = 软件位 OR 硬件位`。硬件位依赖 `PTE_WRITE(DBM)=1` 且 `PTE_RDONLY=0`。

`pgtable.h:311` 的注释给出完整真值表：

```
Dirty  Writable | PTE_RDONLY  PTE_WRITE(DBM)  PTE_DIRTY(sw)
  0      0      |   1           0              0
  0      1      |   1           1              0      <- writable-clean
  1      0      |   1           0              1
  1      1      |   0           1              x      <- writable-dirty
```

### 3.2 关键：硬件是"清除 AP[2]"，不是"置位 AP[2]"

**【架构事实】** 以下限于传统直接权限编码，且硬件支持并启用了 dirty-state 更新。
仅设置 DBM 不足以开启该功能。此时 Stage-1 在 DBM=1 时：

- **writable-clean**：DBM=1, AP[2]（即 PTE_RDONLY, bit 7）=1。此时页对硬件表现为
  只读，但软件意图是可写，硬件尚未记录脏。
- 写命中后，**硬件把 AP[2] 清零**（1→0），页变为实际可写，即 writable-dirty。

架构规定硬件对脏位的硬件转换方向是 **清除 AP[2]**（1→0）。**绝不存在硬件"置位
AP[2]"来标记脏。** 软件恢复 writable-clean（清脏）时才是软件去置位 AP[2]（0→1），
见 `pte_mkclean`（`pgtable.h:193`：清 `PTE_DIRTY`、置 `PTE_RDONLY`）。

注意区分两个容易混淆的方向：

| 操作 | 主体 | AP[2] 变化 | 对应代码 |
|---|---|---|---|
| 写命中变脏 | 硬件 | 1→0（清除） | `pte_hw_dirty` 判定 |
| 软件清脏（mkclean） | 软件 | 0→1（置位） | `pte_mkclean` |
| 软件施加写保护（wrprotect） | 软件 | （置位 RDONLY，清 DBM） | `pte_wrprotect` |

**清除 dirty 与 writeprotect 不是一回事。** `pte_mkclean` 只清软件脏位并置
`PTE_RDONLY`，`PTE_WRITE/DBM` 保留；`pte_wrprotect`（`pgtable.h:211`）会先把硬件脏
（`pte_hw_dirty`）转移到软件 `PTE_DIRTY`，再清除 `PTE_WRITE`、置 `PTE_RDONLY`。
前者页仍"逻辑可写"（DBM 在），后者彻底去写。

### 3.3 Stage-2 的 S2AP（若涉及）

**【架构事实】** 传统 Stage-2 直接权限编码的 S2AP[1:0] 位于描述符 bit[7:6]：
00 无访问、01 只读、10 只写、11 读写。支持并启用硬件 dirty-state 管理且 DBM=1 时，
硬件通过 **S2AP[1] 从 0 置为 1** 标脏；对于可读映射，典型编码从 01 变为 11。
软件清脏则反向清除该位，并完成所需维护。不能把 Stage-1 的 AP[2] 变化方向套用到
Stage-2。本文后面的 CPU contpte 分析是 Stage-1 路径，不据此宣称本仓库支持
Stage-2 CONT dirty logging。

---

## 4. CPU 侧各操作详解

> 下文假设持有 PTL（页表锁）的情形，除非单独说明无锁。PTL 提供的是"软件对软件"
> 的串行化，**不保证**原子地屏蔽硬件对单个 PTE 的并发更新。硬件仍可能并发地清除
> AP[2] 或置位 AF。因此涉及可能被硬件改的位时，代码按操作使用原子交换或
> `cmpxchg_relaxed` 重试循环；有意同时清除 young/dirty 的路径另有直接写入优化。

### 4.1 read（contpte_ptep_get, contpte.c:287）

持锁读。先把 `ptep` 对齐到组首，逐个 `__ptep_get` 扫描组内 PTE，把任意成员里的
dirty/young **OR 进传入的 `orig_pte` 返回副本**。

要点：

- **OR 是合成的，不回写。** 函数只修改本地返回值，不写任何 PTE。所以"读"不会把
  dirty 抹平或传播到组内其他成员。
- 只在找到第一个 dirty 后继续扫到首个 young（或反之）即 break，不保证遍历完整。
  这是优化，因为只要有一个 dirty 和一个 young 就足以合成"已脏已访问"。
- 返回的副本承载合并后的 dirty/young，**不反映具体是组内哪个 PTE 贡献了脏**。
  架构与实现都不承诺精确的子页写日志。

### 4.2 fold（contpte_convert via __contpte_try_fold, contpte.c:208/49）

折叠调用 `contpte_convert`（`contpte.c:49`）：

1. 对齐到组首，构造期望 PTE（pfn 与 prot 对齐）。
2. 逐个 `__ptep_get_and_clear`（底层是 `xchg_relaxed` 归零，`pgtable.h:1009`）取出
   组内每个 PTE，**逻辑 OR 累积 dirty/young** 到目标 `pte`（`contpte.c:64`）。
3. 根据 BBML3 决定是否做中间 TLBI（见第 6 节）。
4. `__set_ptes` 逐项重建**全部组内 PTE**，属性模板相同、PFN 依次递增。
   这不是整组原子写入，也不是把所有项写成相同 PFN。

所以"convert 是 OR 后重画整组"，保守地把脏保留进所有重建描述符。期间组内 PTE
短暂全为 0（invalid），由中间或最终 TLBI 保证一致。

### 4.3 unfold（contpte.c:272）

`__contpte_try_unfold` 清掉 CONT 位后同样走 `contpte_convert`，把一个 CONT 块拆成
N 个不带 CONT 的普通 PTE，地址映射仍连续。同样先 get_and_clear 全组、OR 累积、
再 `__set_ptes` 重画。
unfolding 是后续 partial 修改（如 partial writeprotect、改 DBM）的前置步骤。

### 4.4 lockless read（contpte_ptep_get_lockless, contpte.c:344）

无锁读。挑战在于：无法原子地读完整组，且组可能被并发 unfold/refold。

**【Linux 选择】** 策略是"读后校验、不一致就重试"（`contpte.c:370` `retry:`）。
对扫描到的 PTE 调 `contpte_is_consistent` 校验 valid+cont、pfn 连续、prot 一致
（忽略 dirty/young）。一旦发现不一致就 `goto retry`，重新读取目标项并开始扫描。
找到所需 dirty/young 后可能提前结束，不保证校验全部成员。

注意：这是**重试型一致视图，不是原子快照**。函数不承诺读到的组在某一瞬间是
"冻结"的，也不能推导出返回值对应某一时刻的全组原子快照。脏/young 同样合并进返回副本。

### 4.5 clear young/dirty（contpte_clear_young_dirty_ptes, contpte.c:550）

若范围起点或终点落在 CONT 块中间，函数会把范围**扩展到覆盖整组边界**
（`contpte.c:567`），再调 `__clear_young_dirty_ptes`。依赖同一 folio 前提，整组
清脏/清 young 等价于 folio 级语义，无需 unfold。

底层 `__clear_young_dirty_ptes`（`pgtable.h:1112`）区分两种情形：

- **同时清 young 与 dirty**（`flags == (CYDP_CLEAR_YOUNG | CYDP_CLEAR_DIRTY)`）：
  直接 `__set_pte(ptep, pte_mkclean(pte_mkold(pte)))`，即直接覆写，不走 cmpxchg。
- **只清其中之一**：走 `__clear_young_dirty_pte`（`pgtable.h:1093`）的 `cmpxchg_relaxed`
  重试循环，以保留硬件对另一个状态的并发更新；硬件更新的是 AF/AP[2]，不是 DBM。

辅助函数本身**不发 TLBI**。该接口的调用方负责所需维护，例如 `mm/madvise.c:775`
在清 young/dirty 后调用 `tlb_remove_tlb_entries`，纳入后续 gather 刷新。
另一条独立的 young-only 接口 `contpte_ptep_clear_flush_young` 在 `contpte.c:524`
有 young 时发 `__flush_tlb_range_nosync` 覆盖整组，但有意省略尾随完成屏障。

注意 `contpte_ptep_test_and_clear_young`（`contpte.c:486`）同样是整组清 young，
注释说明 core-mm 实际按 folio 而非按页追踪 young/dirty，所以整组清是安全的。

### 4.6 writeprotect（contpte_wrprotect_ptes, contpte.c:532）

- **整组写保护**：可直接写保护，不动 CONT 位。靠后续 mmu_gather 的 TLBI 收尾。
  在 flush 前，页"可能写保护也可能没"，flush 后保证已写保护（`contpte.c:539`）。
- **部分范围写保护**：必须先 `contpte_try_unfold_partial`（`contpte.c:29`）展开边界
  组。因为不能出现"CONT 置位但只写保护了组内一部分 PTE"的状态，那样 flush 后
  行为不可预测（`contpte.c:541`）。

即"实际的部分写保护会 unfold，而整组写保护可以保持 CONT"。

底层 `__ptep_set_wrprotect`（`pgtable.h:1078`）用 `pte_wrprotect` 在 cmpxchg 循环里
转换：先把硬件脏转存为软件 `PTE_DIRTY`，再清 DBM、置 RDONLY（`pgtable.h:217`）。

### 4.7 set_access_flags（contpte_ptep_set_access_flags, contpte.c:579）

先读出整组视图比较是否真有变化，无变化直接返回 0（`contpte.c:592`）。

- **写位（PTE_WRITE/DBM）不变**：可就地改 access/dirty 标志而不 unfold（`contpte.c:599`）。
  对整组逐个调用 `__ptep_set_access_flags(..., 0)`，禁止内部逐页 TLBI；最后仅在
  参数 `dirty` 非零时调用 `__flush_tlb_range` 覆盖整组（`contpte.c:614`）。即"只改访问标志、写位不变时
  组可保持 CONT 并整组更新"。
- **写位变化**：必须 unfold（`contpte.c:617`），因为 DBM 的逻辑写入语义会改变组
  的一致性前提。

底层 `__ptep_set_access_flags`（`fault.c:242`）从输入模板仅保留 RDONLY、AF、WRITE、
DIRTY 这几个待合并状态位（`fault.c:253`），不会清除当前项的 PFN 等其他字段；
它用 cmpxchg 与硬件对 AF/RDONLY 的并发更新
竞争，并在 `dirty` 为真时 `flush_tlb_page`（`fault.c:272`）淘汰旧的只读 TLB 项。

---

## 5. 完整生命周期示例

以 4K 页、`CONT_PTES=16`、组大小 64K 为例，说明正常追踪周期。
下列标记中的 clean/dirty 是脏状态，不应把 writable-clean 误认为真正写保护。

### 5.1 折叠（fold）

```
初始（同一 folio，地址连续对齐，逻辑写权限一致，尚未设置 CONT）:
[clean,n][clean,n] ... [dirty,n][clean,n]   (16 项)

1) contpte_convert: get_and_clear 全组（底层 xchg 归零），OR 累积 dirty/young:
[ 0 ][ 0 ] ... [ 0 ][ 0 ]

2) （BBML3 可省略）中间 TLBI + DSB

3) __set_ptes 重建整组，传播汇总 dirty，PFN 递增:
[dirty,c][dirty,c] ... [dirty,c][dirty,c]   (16 项全 CONT)

4) 软件清脏整组，并完成该追踪路径所需的 TLB 维护:
[clean,c][clean,c] ... [clean,c][clean,c]
每项 DBM=1、AP[2]=1、软件 PTE_DIRTY=0
```

折叠要求去除 dirty/young 后各项属性已经一致，不会擅自把不同逻辑写权限折叠成一组。
已有脏状态先被保守传播；只有在调用方已处理旧脏信息、允许清脏后，才能开始下一周期。

### 5.2 写命中（硬件）

硬件 dirty 更新已启用的情况下，后续写访问可触发 AP[2] 清除（RDONLY 1→0），
相应描述符变 writable-dirty。
**【未规定行为】** 组内其余 PTE 的 AP[2]/AF 状态不保证同步更新。硬件可能只更新
命中的那一个 PTE，组内成员因此可能出现不等。软件不依赖"组内 PTE 始终相等"。

### 5.3 读回（ptep_get）

`contpte_ptep_get` 扫描全组，把任意成员的 dirty/young OR 进返回副本。即使组内只有
一个 PTE 实际被硬件标脏，返回的视图也是"脏"。不回写，不改变组内分布。

### 5.4 部分写保护（partial wrprotect）

若只写保护组内一部分页，`contpte_wrprotect_ptes` 先 `contpte_try_unfold_partial`
展开边界组，再 `__wrprotect_ptes`。展开后组变回 16 个不带 CONT 的普通 PTE，可逐页写保护。

### 5.5 整组写保护（full wrprotect）

若写保护覆盖整组，不 unfold，直接 `__wrprotect_ptes` 整组，靠后续 mmu_gather
TLBI 收尾。组保持 CONT。

### 5.6 清脏/清 young（clear_young_dirty）

`contpte_clear_young_dirty_ptes` 把范围扩到组边界后调底层清。同时清两者时直接
覆写，只清其一走 cmpxchg。不发 TLBI（由上层决定）。

---

## 6. TLBI 与 erratum

### 6.1 BBML3 与中间 flush

`contpte_convert`（`contpte.c:202`）只在 `!system_supports_bbml3()` 时才发中间
`__flush_tlb_range`。`system_supports_bbml3`（`cpufeature.h:942`）依赖
`ARM64_HAS_BBML3` 替代 cap。

**【架构能力与 Linux 使用方式】** 按该函数注释讨论的 BBML3 场景，contiguous
位误编程不会触发
TLB Conflict Abort，只会产生某个已缓存 TLB 项的 OA/权限/属性，绝不 amalgamate。
Linux 据此在这些转换路径省去中间 TLBI。注释中的折叠例子依赖最终刷新清除旧目标
项及新 CONT 项，其他未改变映射的旧项可自然淘汰；展开例子依赖最终刷新清除旧
CONT 项和目标普通项。不能据此推广为任意页表修改都可省略刷新。

无 BBML3 时（BBML0 语义），必须保留中间 TLBI，否则其他 PE 可能在旧(RW,n) 与
新(RO,c) 并存期间看到不可预测的 amalgamation 行为。注释在 `contpte.c:71` 起详细
图解了 fold 与 unfold 两种情形。

### 6.2 TLBI 的地址覆盖而非单条命令强制

CONT 组的 TLBI 维护，**架构要求的是地址覆盖整组**，并不强制"必须发一条覆盖整组
的命令"或"必须逐页发"。Linux 用 `__flush_tlb_range(..., 3)` 这类 last-level
粒度的范围失效，覆盖组对应的地址区间即可。具体发几条、用哪种 TLBI 操作是实现与
erratum 约束的产物。

### 6.3 MMU-700 erratum 3777127

**【特定硬件勘误，不是通用架构要求】** 该 erratum（SDEN-1786925，Category B）描述：在使用
contiguous 页表项时，TBU 可能出现 under-invalidation。规避方式是**用一条覆盖整组
的 range invalidation，或把失效序列重复两遍**；SYNC 不需要做两次。

**【核验边界】** 按编号检索未找到显式标注 `3777127` 的实现，但编号未命中不能证明
不存在等效规避措施。本文既不声称本仓库已有完整 workaround，也不声称当前代码已
对该勘误证明安全。实际适用版本及规避策略应核对对应芯片勘误和驱动路径。

---

## 7. CPU vs SMMU

### 7.1 SMMU 有独立的 HTTU，不复用 CPU contpte.c

SMMUv3 有自己的 HTTU（Hardware Translation Table Update）能力：HA（Hardware Access flag）
与 HD（Hardware Dirty）。**SMMU 有独立的 walker 与控制能力，不调用 CPU 侧的
`arch/arm64/mm/contpte.c` 管理其 io-pgtable 页表。** 普通 DMA 域使用独立页表；
SVA 等场景可以共享进程页表，不能笼统说任何情况下都是两套页表。Stage-1 的基本
脏位编码可以一致，但能力启用、同步和软件记账路径不同，需检查 HTTU 支持及 HA/HD 配置。

### 7.2 SMMU Stage-1 的脏位编码（io-pgtable-arm.c）

SMMU Stage-1 PTE 同样用 DBM(bit 51) + AP[2](RDONLY, bit 7) 的组合（
`io-pgtable-arm.c:90,107`）：

- `ARM_LPAE_PTE_AP_WR_CLEAN_MASK = ARM_LPAE_PTE_AP_RDONLY | ARM_LPAE_PTE_DBM`（
  `:111`）
- `iopte_writeable_dirty(pte)`：`(pte & WR_CLEAN_MASK) == DBM`，即 DBM=1 且
  RDONLY=0（`io-pgtable-arm.c:170`）
- `iopte_set_writeable_clean(ptep)`：`set_bit(AP_RDONLY_BIT)`，即软件置 RDONLY
  （AP[2] 0→1）恢复 writable-clean（`:173`）

这与 CPU 侧 Stage-1 的硬件清 AP[2]、软件置 AP[2] 方向一致。

### 7.3 SMMU dirty log 路径与 CONT 的当前行为

`io_pgtable_visit_dirty`（`io-pgtable-arm.c:1325`）在叶子 PTE 处：

```c
if (iopte_writeable_dirty(pte)) {                 // :1335
    if ((pte & ARM_LPAE_PTE_CONT) &&
        !(walk_data->flags & IOMMU_DIRTY_NO_CLEAR))
        return -EOPNOTSUPP;                        // :1338
    iommu_dirty_bitmap_record(...);
    if (!(walk_data->flags & IOMMU_DIRTY_NO_CLEAR))
        iopte_set_writeable_clean(ptep);           // :1342
}
```

要点：

- **遇到 dirty 且带 CONT 的叶子，且要求清脏时，直接返回 `-EOPNOTSUPP`。**
  这里分析的 `read_and_clear_dirty` 扫描路径拒绝该操作，不能扩大为所有脏日志接口
  都具有完全相同的行为。
- 只读路径（`IOMMU_DIRTY_NO_CLEAR`）只记录 leaf 的 dirty 位，**不做组聚合**，按
  单个 leaf 记账。
- `iopte_set_writeable_clean` 用 `set_bit` 直接改单个 PTE 的 RDONLY 位。

**【Linux 选择 / 本仓库现状】** SMMU 的同一 folio 假设**不被保证**。DMA 映射的
物理页不一定来自同一 folio，CONT 组的"同一 folio"前提在 SMMU 侧并不天然成立。
当前 `visit_dirty` 对 dirty CONT 清脏返回 `-EOPNOTSUPP`，是一种保守拒绝。本文
不声称该路径已有修复或已证明安全。

### 7.4 对比小结

| 维度 | CPU（arm64 contpte） | SMMU（io-pgtable-arm） |
|---|---|---|
| CONT 软件管理 | `contpte.c` 全套 fold/unfold/convert | 不复用；PTE 自管 CONT 位 |
| 同一 folio 前提 | 折叠前强制检查 | 不保证 |
| dirty CONT 清脏 | `contpte_clear_young_dirty_ptes` 扩大到整组 | 上述 `visit_dirty` 路径拒绝清除 dirty CONT |
| 硬件脏方向 | Stage-1 硬件清 AP[2] | Stage-1 同；传统 Stage-2 硬件置 S2AP[1] |
| 并发操作 | PTL 配合 xchg/cmpxchg 等操作 | 示例清脏用 set_bit；完整并发契约须结合对应调用路径 |

---

## 8. 关键辨正汇总

阅读和实现时应重点区分以下问题：

1. **硬件清 AP[2]，从不说"硬件置 AP[2]"。** Stage-1 写命中由硬件把 AP[2]（RDONLY）
   从 1 清到 0 标脏。软件清脏才是置 AP[2]（0→1）。Stage-2 若涉及，软件清脏把
   S2AP[1] 从 1 清回 0；Stage-2 硬件标脏的方向是 0→1。
2. **PTE_WRITE 别名 DBM**（`pgtable-prot.h:16`），`PTE_DIRTY` 是软件 bit55。
3. **`pte_dirty = 软件位 OR 硬件位`**（`pgtable.h:121`）。
4. **不要断言"更新总是只改命中 PTE"或"总是整组"或"任意无关 PTE"。** 硬件可能只更新
   命中 PTE 使组内不等；软件路径按操作类型决定是整组还是边界展开。
5. **不要声称精确组内基础页写日志。** read/lockless 合并出组的"是否脏"视图，
   不保留哪个基础页实际写过的信息。
6. **读的 OR 是合成、不回写。** `contpte_ptep_get` 只改返回副本。
7. **convert 是 OR 后重画整组。** get_and_clear 全组累积 dirty/young，再 `__set_ptes`
   重画全部成员，保守保留脏。
8. **清脏 ≠ 写保护。** `pte_mkclean` 保留 DBM 只置 RDONLY；`pte_wrprotect` 清 DBM。
9. **TLBI 归属与 BBML3 条件中间 flush。** `contpte_convert` 仅在无 BBML3 时发中间
   TLBI；有 BBML3 省略，靠最终 flush 淘汰新旧 TLB 项。
10. **lockless 是重试一致视图，非原子快照。**
11. **同一 folio 是这里 CPU 透明 CONT 管理的关键软件前提，不是通用架构要求。**
12. **set_access_flags 写位不变可保持 CONT 整组改；写位变化须 unfold。**
13. **partial writeprotect 展开，full writeprotect 可保持 CONT。**
14. **清 young/dirty 扩组边界、辅助函数不发 TLBI；同时清两者直接覆写，只清其一走
    cmpxchg。**
15. **TLBI 是地址覆盖要求，非单条命令强制。**
16. **MMU-700 3777127：单条覆盖整组的 range invalidation，或重复失效两遍；SYNC 无需
    两次。不根据编号检索结果断言有无等效 workaround，不声称代码已证安全。**
17. **SMMU 独立 HTTU（HA/HD），不复用 CPU contpte.c，同一 folio 不保证。**
18. **`visit_dirty`（:1335）对 dirty CONT 清脏返回 `-EOPNOTSUPP`，只读路径按 leaf
    记账不聚合组，不声称已有修复。**

---

## 9. 参考链接

ARM 官方文档（权威来源，非本仓库）：

- ARM ARM, D8.7.1 The Contiguous bit:
  https://support.arm.com/documentation/ddi0487/md/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D8-The-AArch64-Virtual-Memory-System-Architecture/-D8-7-Other-descriptor-fields/-D8-7-1-The-Contiguous-bit?lang=en
- Access Flag / Dirty state（HTTU 说明）:
  https://developer.arm.com/documentation/102376/latest/Access-Flag/Dirty-state
- SMMU architecture spec IHI0070:
  https://support.arm.com/documentation/ihi0070/ha/
- SMMU programming, Command queue:
  https://support.arm.com/documentation/109242/0100/Programming-the-SMMU/Command-queue?lang=en
- SDEN-1786925, Errata 3777127（contiguous 页表项 under-invalidation）:
  https://support.arm.com/documentation/SDEN-1786925/0000/Errata-descriptions/Category-B/3777127-Under-invalidation-in-TBU-possible-when-using-contiguous-page-table-entries

本地源码索引（行号对应本仓库当前代码）：

- `arch/arm64/mm/contpte.c`: convert(49), fold(208), unfold(272), get(287),
  lockless(344), wrprotect(532), clear_young_dirty(550), set_access_flags(579)
- `arch/arm64/include/asm/pgtable.h`: dirty 判定(119), mkclean(193), mkdirty(201),
  wrprotect(211), ptep_get_and_clear/xchg(1006), clear_young_dirty helpers(1093),
  公共封装(1368)
- `arch/arm64/include/asm/pgtable-prot.h`: PTE_WRITE/DBM(16), PTE_DIRTY(18)
- `arch/arm64/include/asm/pgtable-hwdef.h`: CONT_PTES/CONT_PTE_SIZE(77)
- `arch/arm64/mm/fault.c`: __ptep_set_access_flags(242)
- `arch/arm64/Kconfig`: ARM64_CONT_PTE_SHIFT(302)
- `arch/arm64/include/asm/cpufeature.h`: system_supports_bbml3(942)
- `drivers/iommu/io-pgtable-arm.c`: CONT 定义(97), AP/HAP(107), writeable_dirty(170),
  set_writeable_clean(173), visit_dirty(1325), dirty 拒绝分支(1335)
