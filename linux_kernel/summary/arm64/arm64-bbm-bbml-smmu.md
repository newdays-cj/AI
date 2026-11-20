# ARM64 BBM、BBML 与 SMMU 支持机制

## 1. 概述

Break-Before-Make（BBM）是一种页表更新协议：先撤销旧映射，完成必要的转换缓存失效，再建立新映射，以避免不受支持的新旧转换同时被使用。

Break-Before-Make Level（BBML）描述硬件在改变映射粒度时，允许软件采用多宽松的更新流程。它主要影响 Block 与 Table 的替换、小页与大页的转换，以及 Contiguous 映射的转换。

需要区分以下概念：

| 概念 | 含义 |
|---|---|
| BBM | 更新页表时先 Break、后 Make 的协议 |
| BBML | 硬件对放宽映射粒度更新流程的支持等级 |
| TLBI | 使缓存地址转换失效的维护操作 |
| TLB conflict abort | 硬件遇到冲突转换缓存状态时可能报告的异常 |
| 数据 Cache 维护 | 对缓存数据执行 clean/invalidate，与 TLB 失效不同 |

**BBML 放宽的是特定条件下的页表更新约束，不代表所有有效描述符可以任意覆盖，也不代表可以省略所有 TLBI、屏障和设备侧同步。**

CPU 与 SMMU 各自报告能力。即使共享同一份页表，也必须分别满足两类转换硬件的更新要求。

## 2. 页表更新为什么需要 BBM

### 2.1 修改页表内存不会自动更新全部转换缓存

一次地址转换可能涉及：

```text
输入地址：VA / IOVA / IPA
              |
              v
最终转换缓存（TLB）
              |
         未命中时遍历
              |
              v
中间级转换缓存 / walk cache
              |
              v
内存中的页表描述符
              |
              v
输出地址、访问权限、内存属性
```

软件修改内存中的描述符后，已有 TLB 项和中间级缓存不一定立即消失。其他 CPU、SMMU 或设备可能继续使用旧转换，也可能通过新的遍历获得新转换。

单次原子地写入描述符，只能避免描述符被读取为撕裂值，不能解决旧转换仍在其他缓存中存在的问题。

### 2.2 新旧转换共存可能破坏的保证

不受支持的更新方式可能导致：

- 同一输入地址出现多个重叠的缓存转换。
- 不同观察者使用不同输出地址或内存属性。
- 内存一致性、单拷贝原子性或内存顺序受到破坏。
- 单处理器语义不再成立。
- 独占访问监视器未按架构要求清除。
- 产生 TLB conflict abort。

BBM 通过在安装新映射之前撤销并清除旧转换，建立安全的转换边界。

### 2.3 BBM 与锁、原子操作的分工

| 机制 | 主要解决的问题 |
|---|---|
| 页表锁 | 软件修改者之间的并发协调 |
| 原子描述符操作 | 防止描述符撕裂，协调并发的软件或硬件位更新 |
| BBM | 满足需要撤销旧转换的架构更新协议 |
| TLBI 与完成同步 | 清除相关缓存转换，并确定何时失效已经完成 |
| 页表内存生命周期管理 | 防止 walker 或软件访问已释放的页表 |

持有页表锁并不会阻止其他 CPU 或 SMMU 使用已有 TLB 项；完成 BBM 也不会自动替代软件侧的锁和生命周期管理。

## 3. 哪些更新需要考虑 BBM

### 3.1 典型场景

在多个执行线程可能使用相同页表等适用条件下，架构规定需要重点处理以下变化：

| 更新类型 | 处理要点 |
|---|---|
| 改变 Memory Type、Shareability、Cacheability | 在本节适用条件下需要 BBM，并可能需要额外的数据 Cache 维护 |
| 改变输出地址 OA | 当新旧任一映射可写，或新旧物理位置内容不同等条件成立时，需要 BBM |
| 改变 Block/Table 映射粒度 | 按 BBML 能力选择传统 BBM 或架构允许的替代流程 |
| 创建可能与旧 non-global 转换重叠的 global 转换 | 需要相应 BBM 和失效处理 |

OA 的变化也包括架构定义的输出地址空间变化。

这些是架构规则的概括。具体更新还需要结合 translation stage、启用的特性及描述符字段定义判断。

### 3.2 权限更新不等于一定需要完整 BBM

例如，在不引入其他 BBM 条件的情况下，单独将访问权限从 RW 改成 RO，不能简单归类为“必然需要完整 BBM”。但必须清除旧的可写缓存转换，并在依赖新权限之前等待相应维护完成。

```text
页表已经是 RO
    不等于
所有 CPU、SMMU 和设备已无法使用旧 RW 转换
```

类似地，建立此前无效的新映射、修改 AF、清除或收集 Dirty 状态，都有各自的更新规则，不能统一套用“所有修改都 BBM”或“只写 PTE 即可”的结论。

启用硬件 AF/Dirty 更新时，还必须协调硬件对描述符的并发修改，避免软件覆盖硬件更新，或错误归属访问、脏状态。

### 3.3 改变内存属性还可能需要数据 Cache 维护

BBM/TLBI 处理的是地址转换，不会自动清理映射数据的缓存副本。

例如，从 Write-Back 改成 Non-cacheable 等变化，可能要求对受影响的数据执行适当的 clean/invalidate。改变 Shareability 也有专门的架构流程。

因此，“完成 TLBI”不能作为“所有内存属性切换工作都已完成”的依据。

### 3.4 Arm ARM 的判定规则：按语义判断，不是固定的位掩码

Arm ARM DDI 0487 的 “Using break-before-make when updating translation table entries” 中，规则 **R_WHZWS** 规定：多个执行线程使用同一套转换表时，如果更新涉及以下任一情况，软件必须采用 BBM，除非对应条款明确允许替代流程：

1. 改变 Memory Type、Shareability 或 Cacheability。
2. 改变转换的输出地址 OA，并且满足以下至少一个条件：新旧转换任一允许写入，或新 OA 对应的内容与旧 OA 不同。可写性包括 `DBM=1` 且启用硬件 Dirty 更新时允许写入的情况，不能只看描述符当下是否显示 RO。
3. 改变转换粒度，包括 Table → Block 和 Block → Table；如果不采用 FEAT_BBML1 允许的流程，且未实现 FEAT_BBML2，就需要 BBM。
4. 创建可能与 TLB 中 non-global 项重叠的 global 项。

同节 **I_WZRHR** 明确说明：输出地址空间的变化也属于 OA 变化。例如，适用转换机制中改变 Secure/Non-secure 输出空间，不能只因地址数值不变就视为 OA 未变化。

因此，不能用一条脱离配置的规则表示“以下位只要变化，就全部需要 BBM”。必须同时检查描述符种类、转换阶段、有效属性、旧缓存的作用域，以及启用的架构特性。

以下位表采用常规 **64 位描述符、4 KiB granule、最多 48 位输出地址**的布局，并假设未启用会改变相关字段解释的 LPA2、D128、S1PIE/S2PIE、S1POE/S2POE、AIE 等扩展。Stage 2 的 XN 示例不启用 XNX。扩展打开后，应重新对照对应格式，不能照抄位号。

### 3.5 Stage 1：描述符位与 BBM 条件的对应关系

下表主要针对 Block/Page 叶子描述符。表中的 `[n:m]` 表示描述符中的位置；例如描述符 `[7:6]` 对应架构名称 `AP[2:1]`，而不是 `AP[1:0]`。

| 字段 | 描述符位 | 更新时是否需要 BBM | 说明 |
|---|---|---|---|
| `AttrIndx[2:0]` | `[4:2]` | 改变有效内存类型或 Cacheability 时需要 | 该字段索引 `MAIR_ELx`。应比较实际选中的属性，不能只比较索引数字 |
| `NS` | `[5]` | 改变有效输出地址空间时，按 OA 变化条件判断 | 只在该转换机制赋予此位相应含义时适用；某些机制下该位被忽略或另有定义 |
| `AP[2:1]` | `[7:6]` | 单独改变访问权限，不由 R_WHZWS 自动要求完整 BBM | 仍需对应失效和同步；启用硬件 Dirty 管理时要结合 DBM 判断 |
| `SH[1:0]` | `[9:8]` | 改变有效 Shareability 时需要 | 还可能需要额外数据 Cache 维护；本表不适用于 LPA2 复用这些位的格式 |
| `AF` | `[10]` | 不能将 AF 更新统一归为必须 BBM | 设置、清除和重新采样 AF 有相应更新及失效规则，还需处理硬件并发更新 |
| `nG` | `[11]` | `1 → 0` 且新 global 项可能与旧 non-global 项重叠时需要 | `0 → 1` 不属于这条相同的触发条件，但仍需清理旧 global 项，不能假设按 ASID 失效已经足够 |
| 输出地址 `OA` | Page 通常为 `[47:12]` | 新旧任一可写，或新旧位置内容不同时需要 | L2 2 MiB Block 的显式地址部分为 `[47:21]`，L1 1 GiB Block 为 `[47:30]`；低位由偏移形成 |
| `DBM` | `[51]` | 不能仅按该位翻转判断 | 它影响有效可写性及硬件 Dirty 更新；不是一个可独立替代 AF/Dirty 协议的通用软件脏位 |
| `Contiguous` | `[52]` | 按整组映射的一致性和 BBML 规则处理 | 涉及可能不同大小的缓存转换，不能当作无条件可原位修改的性能提示；见第 5、6 节 |
| `PXN` / `UXN` | `[53]` / `[54]` | 单独改变执行权限，不由 R_WHZWS 自动要求完整 BBM | 仍需相应 TLB 和执行上下文同步；若同时修改代码内容，还要处理指令缓存一致性 |

对于描述符有效性、类型和下级表地址，还需要单独判断：

| 字段/变化 | 位位置 | 判断要点 |
|---|---|---|
| Valid/类型 | `[1:0]` | 在允许 Block 的非末级，`01` 为 Block、`11` 为 Table；末级的 `11` 为 Page。bit 0 为 0 表示无效，不能只把 `00` 视为无效编码 |
| 有效 → 无效 | bit 0 清零 | 是撤销映射或 BBM 的 Break 部分；要清理旧转换并完成同步，随后才能依赖撤销效果 |
| 无效 → 有效 | 发布合法的新描述符 | 若旧映射已完成必要撤销，不会仅因这一步而重新要求一次完整 BBM；仍要保证新描述符和下级表的可见性 |
| Table ↔ Block | 非末级的类型改变 | 属于粒度变化，受 BBML 条件约束；不能只检查叶子 OA 是否相同 |
| 下级表地址 | Table 中通常为 `[47:12]` | 它是页表指针，不是叶子输出地址。必须检查被替换子树的映射语义，处理相关 walk cache，并延迟回收仍可能被访问的旧表 |

### 3.6 Stage 2：相似规则，不同字段编码

Stage 2 把 IPA 转换成 PA。其维护目标和部分字段解释不同，不能把 Stage 1 的掩码、ASID 失效或 MAIR 索引逻辑直接套用过来。

| 字段 | 常规描述符位 | BBM 判定和注意事项 |
|---|---|---|
| `MemAttr[3:0]` | `[5:2]` | 改变有效 Memory Type 或 Cacheability 时需要 BBM；这不是 Stage 1 的 `AttrIndx`，其解释还受 S2FWB 等配置影响 |
| `S2AP[1:0]` | `[7:6]` | 单独权限变化不自动要求完整 BBM，但必须按 Stage 2 和组合转换规则撤销旧权限；bit 7 是 `S2AP[1]` |
| `SH[1:0]` | `[9:8]` | 改变有效 Shareability 时需要 BBM；需结合组合转换的属性规则判断 |
| `AF` | `[10]` | 按 AF 更新规则及硬件管理条件处理，不能简单归类为所有更新都 BBM |
| 输出地址 `OA` | Page 通常为 `[47:12]` | 按新旧转换可写性及内容差异条件判断；Block 按对应大小对齐 |
| `DBM` | `[51]`，实现相应功能时 | 与硬件 Dirty 管理和 `S2AP[1]` 的状态变化共同解释 |
| `Contiguous` | `[52]` | 按整组约束、BBML 和适用 Stage 2 更新流程处理 |
| `XN` | `[54]`，此处未启用 XNX | 单独执行权限变化不自动要求完整 BBM，但仍有对应失效要求；启用 XNX 后 `[54:53]` 的解释不同 |
| 描述符类型/下级表地址 | `[1:0]` / 通常 `[47:12]` | 同样需要区分粒度变化、子树替换和页表内存生命周期 |

当 Stage 1 与 Stage 2 组合使用时，还可能存在组合转换缓存。仅更新 Stage 2 页表并失效某类叶子项，不能未经分析就认为全部旧组合转换都已清理。

### 3.7 条件和例外的具体示例

**例一：把 RW 映射从 PA_A 改成 PA_B。** 即使 PA_A 与 PA_B 在切换瞬间内容相同，只要新旧任一转换允许写入，就满足 OA 变化的 BBM 条件。不同观察者使用不同物理位置写入，会使数据分叉，内容相同不能消除这种风险。

**例二：把 RO 映射从 PA_A 改成 PA_B，且内容不同。** 两边只读仍不足以豁免 BBM，因为 R_WHZWS 还独立检查新旧内容是否相同。

**例三：新旧 OA 不同，两边真正只读，且内容相同。** 单就 R_WHZWS 的这项 OA 条件而言，不触发强制 BBM；仍需满足其他更新条件、TLBI、AF/Dirty 管理、并发内容维护和旧页生命周期要求。这不是任意“只读换页”操作的通用许可。

**例四：把 `nG=1` 改成 `nG=0`。** 如果 TLB 中仍可能存在覆盖该 VA 的 non-global 转换，就需要按 global/non-global 重叠规则使用 BBM；旧项还可能属于其他 ASID，不能仅按当前 ASID 考虑。

**例五：把 2 MiB Block 拆成 512 个小页，同时替换其中一页的 PA。** BBML2/3 可以放宽符合条件的粒度转换，但不会自动豁免同时发生的 OA 改变。应分别证明结构转换和叶子映射更新合法，必要时分步执行。

**例六：`AP[2]=1`，但 `DBM=1` 且硬件 Dirty 管理允许写入。** 不能仅以当前 AP 编码为 RO，就把该映射作为“新旧均不可写”的例外。Stage 1 的硬件置脏会涉及 AP[2]，Stage 2 则涉及 S2AP[1]。

### 3.8 在要求 BBM 的场景中省略 BBM，会发生什么

后果不是统一的“访问立即异常”，也不是只要随后补一条 TLBI 就没有问题。错误可能在更新与最终失效之间已经发生，之后失效不能撤销已执行的错误读写。

Arm ARM **I_THWDH** 列出正确 BBM 所防止的问题，包括多个重叠 TLB 项、一致性破坏、单拷贝原子性破坏、顺序或单处理器语义破坏，以及独占访问监视器未按要求清除。

| 不正确的更新 | 可能出现的状态或后果 |
|---|---|
| 可写映射直接切换 OA | 不同观察者经同一个输入地址访问不同物理位置；写入分散，读写结果不再满足原有映射语义 |
| 直接切换 Normal/Device 或 Cacheability | 同一物理位置可能被用不同内存属性访问，产生不满足一致性或顺序要求的行为；TLBI 也不能代替数据 Cache 维护 |
| 不按要求改变 Block/Table 或 Contiguous 表示 | 缓存中出现覆盖同一输入地址的不同粒度转换；在架构允许报告的实现上可能出现 TLB conflict abort |
| 不正确地创建 global 项 | 新 global 转换与旧的按 ASID 区分的转换重叠，破坏预期的地址空间转换行为 |
| 过早释放被替换的下级页表 | 在途 walker 或软件读者可能访问已重用的内存；这是还必须单独处理的生命周期错误，BBML 不会自动解决 |

违反更新规则后，不能以“某次实测没有 fault”证明正确。具体行为还受实现及违反的具体规则影响；本文不把所有违规都概括成同一种 UNPREDICTABLE，也不承诺硬件一定产生可恢复的异常。

### 3.9 硬件 AF/Dirty 更新的额外风险

Arm ARM 同节的 **I_GXGZY** 指出，启用硬件 AF 管理的转换阶段，在修改转换项而未遵循 BBM 时，可能出现：

- 对应内存已被访问，但 AF 没有设置。
- 硬件更新描述符与程序顺序中随后 Store 之间的要求顺序未被满足。

**R_RTBRL** 对启用硬件 Dirty 管理的转换阶段指出，可能出现：

- 对应位置没有发生写入，硬件却修改了 `AP[2]` 或 `S2AP[1]`。
- 对应位置发生写入，硬件却没有完成相应位更新。
- 硬件更新与随后 Store 之间的要求顺序未被满足。

这比“软件最后写入时覆盖了 AF/Dirty 位”更广：描述符重用、硬件更新归属及其顺序也需要考虑。对页回收和迁移而言，漏报访问或漏报写入都可能影响正确性。

这些条文不能反向理解为“AF/AP/DBM 的每次合法管理操作都必须完整 BBM”。软件仍应使用针对具体更新允许的协议，协调硬件修改，执行必要的失效和完成同步；原子读改写也不能单独替代整个协议。

### 3.10 实际判定顺序

1. 确认 Stage 1/Stage 2、描述符格式、页大小及启用的扩展，先确定每一位的真实含义。
2. 比较新旧转换语义，检查内存属性、OA、有效可写性、global/non-global 重叠和粒度变化。
3. 若依靠 BBML 放宽粒度更新，确认 CPU、SMMU 等实际使用者均具有所需能力；同时检查其他变化是否仍强制 BBM。
4. 处理硬件 AF/Dirty 并发更新及多描述符组的一致性。
5. 选择覆盖旧转换和相关 walk cache 的维护流程，完成相应可见性与失效同步。
6. 在撤销真正完成、且软件读者和 walker 均满足生命周期要求后，才回收旧页或旧页表。

本节架构条件来自参考资料 1、2、4。常规位号还与工作区 `arch/arm64/include/asm/pgtable-hwdef.h:164` 和 `drivers/iommu/io-pgtable-arm.c:66` 的字段定义交叉核对；这些实现定义用于辅助检查位号，不替代 Arm 架构要求。

## 4. 传统 BBM 的流程与同步

### 4.1 概念流程

以下步骤对应 Arm ARM 的 **R_DDMVT**：写无效项、DSB、广播失效旧转换、DSB 等待完成、写新项、DSB 确保新项可见。

```text
旧有效描述符
       |
写入无效描述符                   Break
       |
DSB：确保无效描述符可见
       |
使旧转换失效
       |
DSB：等待相关失效完成
       |
写入新有效描述符                 Make
       |
DSB：确保新描述符可见
       |
按执行上下文完成必要的其他同步
```

对于常见 CPU Stage 1、Inner Shareable 的情形，可以用以下伪代码理解其顺序：

```text
store invalid descriptor
DSB ISHST
appropriate broadcast TLBI
DSB ISH
store new descriptor
appropriate DSB
context synchronization if required
```

这不是可直接复制到任意内核路径的汇编模板。Stage 2、组合转换、不同共享域、可执行映射和不同缓存一致性条件，可能需要不同的指令、作用域和附加步骤。

### 4.2 各步骤解决的问题

- 第一次 DSB：保证撤销旧映射的写入已经达到要求的可见性。
- TLBI：清除覆盖该映射的相关缓存转换，必要时包括中间级转换缓存。
- 失效后的 DSB：建立架构规定的失效完成边界。
- 发布新描述符及后续屏障：确保后续遍历能正确获得新映射。
- ISB 等上下文同步：按具体上下文处理当前 PE 的指令执行和转换环境，不能代替跨核 TLBI。

TLBI 的范围必须覆盖旧转换。不能仅因为新映射是 4 KiB，就假定旧的更大映射和相关 walk cache 已经得到正确处理。

### 4.3 临时无效窗口

Break 与 Make 之间，页表内存在无效描述符；仍持有旧 TLB 项的访问可能继续使用旧转换，新的遍历则可能遇到无效项。

软件必须协调这个窗口，例如避免依赖正在撤销的映射，或具备适当的故障处理机制。BBM 本身不保证对业务完全无感。

如果更新的是正在执行代码或关键内核数据所依赖的映射，必须证明更新过程不会使自身陷入无法处理的故障。

## 5. 映射粒度变化与 BBML

### 5.1 Block 拆分与小页合并

以 4 KiB translation granule 的常见配置为例：

```text
L2 Block descriptor
        |
        +-- 一个 2 MiB 映射

                  拆分 / 合并

L2 Table descriptor
        |
        +-- L3 页表
              +-- 512 个 4 KiB Page descriptor
```

即使两种表示具有完全相同的地址对应关系和属性，旧 2 MiB 转换与新 4 KiB 转换也可能同时存在于缓存中。

BBML 规定硬件如何处理这种不同粒度转换共存的状态。

### 5.2 Contiguous 映射

Contiguous bit 表示一组满足条件的相邻描述符，可允许硬件将它们作为更大范围缓存。

改变一组描述符的 Contiguous 状态，也可能造成不同粒度的缓存转换共存。因此，不能把 Contiguous bit 当作一个可随意切换、无需同步的普通软件标志。

Contiguous 映射仍由一组描述符组成，并不等同于在上一级安装了一个 Block descriptor。

## 6. ARM64 CPU 的 BBML 等级

CPU 通过只读字段 `ID_AA64MMFR2_EL1.BBM[55:52]` 报告能力。

| 字段值 | 等级 | 主要含义 |
|---|---|---|
| 0 | Level 0 | 对需要 BBM 的粒度变化采用传统 BBM |
| 1 | Level 1 | 提供 nT 等支持，允许使用规定的替代流程 |
| 2 | Level 2 | 粒度变化具有更强正确性保证，无需依赖 nT 过渡流程 |
| 3 | Level 3 | 在 Level 2 基础上，相关粒度变化不产生 TLB conflict abort |

较旧资料通常只列 Level 0～2；包含较新 FEAT_BBML3 的定义增加了 Level 3。

这些放宽主要适用于改变映射粒度、且没有同时引入其他必须 BBM 的变化。把同一个 VA 改为指向无关的 PA，不能仅凭 BBML2/3 就直接覆盖。

### 6.1 Level 1 与 nT

Level 1 允许在规定的 Block/Table 更新流程中使用 nT 属性。

以小页合并为 Block 为例：

```text
1. 将旧 Table descriptor 替换为新 Block descriptor，设置 nT=1
2. 执行必要的 TLB 失效并等待完成，清理旧的小页转换
3. 清除新 Block descriptor 的 nT
4. 再次执行必要的失效，清理仍带 nT 的缓存转换
```

nT 告知硬件需要按相应规则处理潜在的重叠转换。这个例子的优点是整个区间不必先被改为无效映射。

nT 不会自动清理旧 TLB，也不是“无需 TLBI”的开关。上面用于解释合并流程，不是所有拆分、合并操作的通用模板。

### 6.2 Level 2 的保证和限制

对于符合条件的粒度变化，Level 2 保证不会破坏一致性、顺序和单处理器语义，也不会导致应当发生的独占访问监视器清除被遗漏。

但 CPU Level 2 仍可能报告 TLB conflict abort。因此，软件如果想在任意内核上下文利用宽松更新，还必须考虑异常是否可以安全处理、是否可能递归故障。

### 6.3 Level 3 与最终失效

Level 3 增加相关粒度变化不产生 TLB conflict abort 的保证。

它不保证旧转换立刻消失。例如，变换 Contiguous 表示时同时将某页 RW 改为 RO，最终 TLBI 完成前，仍可能使用旧 RW 缓存转换。

软件只能在适当的最终失效和同步完成之后，依赖新权限或回收旧资源。

## 7. SMMUv3 如何支持 BBML

### 7.1 独立能力报告

SMMUv3 使用 `SMMU_IDR3.BBML[12:11]` 报告自己的能力：

| 编码 | 能力 |
|---|---|
| 0b00 | BBML0 |
| 0b01 | BBML1 |
| 0b10 | BBML2 |

不能将 CPU 的 Level 3 编码直接套用到 SMMU 字段。

这是硬件能力标识，不是使能开关。驱动根据能力选择合法流程，SMMU 的页表遍历器和转换缓存提供相应保证。

### 7.2 CPU Level 2 与 SMMU Level 2 的差异

CPU BBML2 仍允许相关粒度变化产生 TLB conflict abort；SMMUv3 BBML2 对该问题有更严格要求，相关受支持的更新不会产生 TLB conflict。

因此，从 Linux 共享页表更新所需的保证看，常见匹配关系是：

```text
CPU：  BBML3，或具备对应无 conflict abort 保证的实现
SMMU：BBML2
```

两边等级数字不同，不表示能力不兼容，也不表示 SMMU 的所有行为都可与 CPU 等级逐项等同。

### 7.3 支持 BBML2 时拆分 Block 的概念流程

假设只改变粒度，保持地址对应关系和属性不变：

```text
1. 分配新的下级页表
2. 填好全部小页描述符
3. 确保新页表内容对 SMMU walker 可见
4. 将上级 Block descriptor 替换为 Table descriptor
5. 完成该更新所需的转换缓存失效与完成同步
6. 在安全条件满足后进行后续属性修改或资源回收
```

对符合规定的更新，BBML2 使软件能够避免传统的：

```text
Block → Invalid → 等待失效完成 → Table
```

而采用：

```text
Block → Table
```

过渡期间可能使用旧 Block 转换，也可能得到新 Page 转换。硬件必须满足该能力规定的正确性保证。

### 7.4 SMMU 仍然需要的维护

| 对象 | 维护目的 |
|---|---|
| 页表内存 | 保证 walker 看到完整的新描述符和下级页表 |
| SMMU TLB / walk cache | 清理旧地址、权限及结构缓存 |
| 命令队列 | 提交对应 stage、ASID/VMID 和范围的失效命令 |
| 命令完成同步 | 确认失效达到规定的完成条件 |
| 设备 ATC | 使用 ATS 时清理设备自身缓存的转换 |
| 旧页表内存 | 防止在途遍历或软件访问已回收的页表 |

SMMUv3 通常通过 `CMD_TLBI_*` 提交转换失效，并根据流程使用 `CMD_SYNC` 完成同步。设备 ATC 的维护则涉及相应的 `CMD_ATC_INV` 流程及完成要求。

CPU 的 DSB 不能代替 SMMU 命令队列完成等待；SMMU 的 CMD_SYNC 也不能代替发布页表之前所需的内存可见性处理。

部分平台支持广播 TLB maintenance 等协同机制。具体实现可减少显式软件操作，但不能默认一次 CPU TLBI 已经清理全部 SMMU 和设备缓存。

BBML 讨论的是转换表更新，不可替代 Stream Table Entry、Context Descriptor 等配置结构自己的更新规则和配置缓存失效流程。

## 8. SVA 为什么要求 CPU 与 SMMU 能力兼容

普通 DMA 场景通常使用独立页表：

```text
CPU：  VA   → CPU 页表   → PA
设备：IOVA  → IOMMU 页表 → PA
```

SVA（Shared Virtual Addressing）允许设备使用进程虚拟地址，SMMU 直接使用进程页表：

```text
CPU 访问 ───────┐
               ├── 同一份进程页表
设备经 SMMU ───┘
```

如果 CPU 侧依靠 BBML3 省略中间维护，SMMU 必须能够安全接受相同的更新过程。仅证明更新对 CPU 合法是不够的。

共享页表也不等于共享 TLB。页表更新后的失效传播仍需要覆盖 CPU、SMMU 以及使用 ATS 时的设备转换缓存。

## 9. 当前 Linux 源码中的实现

以下路径与行号基于本次核对的 `/home/chenjun/Git/linux` 工作区，后续代码变化可能使行号移动。

### 9.1 CPU 能力识别

`arch/arm64/kernel/cpufeature.c:2161` 中的 `cpu_supports_bbml3()`：

- 读取 `ID_AA64MMFR2_EL1.BBM`，识别 Level 3。
- 对部分未通过该字段报告能力、但具有相应保证的实现，使用 MIDR 列表识别。

系统级调用方通过 `system_supports_bbml3()` 判断是否可以使用该能力，不能仅依据某个 CPU 的未经系统协调的寄存器值任意选择更新流程。

### 9.2 Contiguous PTE 优化

`arch/arm64/mm/contpte.c:222`：

```c
if (!system_supports_bbml3())
        __flush_tlb_range(&vma, start_addr, addr, PAGE_SIZE, 3,
                          TLBF_NOWALKCACHE);

__set_ptes(mm, start_addr, start_ptep, pte, CONT_PTES);
```

BBML3 下省略的是中间的一次 TLB flush。该路径仍先清除并汇总旧 PTE 的 AF/Dirty 状态，再写入新的描述符。

因此，这段实现不能理解为“完全没有临时无效项”或“完全不刷新 TLB”。

`arch/arm64/mm/contpte.c:138` 起的注释明确说明，新旧缓存转换可以暂时共存，最终 TLBI 和 DSB 仍用于建立页表更新完成后的所需状态。

### 9.3 SMMU 能力识别

`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h:64`：

```c
#define IDR3_BBM GENMASK(12, 11)
```

`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:5197`：

```c
if (FIELD_GET(IDR3_BBM, reg) == 2)
        smmu->features |= ARM_SMMU_FEAT_BBML2;
```

### 9.4 SVA 兼容性检查

`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3-sva.c:212`：

```c
if (system_supports_bbml3())
        feat_mask |= ARM_SMMU_FEAT_BBML2;

if (system_supports_haft())
        feat_mask |= ARM_SMMU_FEAT_HAFT;

if ((smmu->features & feat_mask) != feat_mask)
        return false;
```

当前实现中，CPU 系统使用 BBML3 能力时，SMMU 若不具备要求的 BBML2，则不满足 SVA 支持条件。

`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3-sva.c:136` 中的 MMU notifier 回调，会调用 `arm_smmu_domain_inv_range()`，将共享页表的失效需求传递给 SMMU。

### 9.5 能力识别与实际利用的边界

本次检查没有发现通过 BBML 专用 quirk 将能力传给通用 ARM LPAE io-pgtable 更新路径的实现。

因此，可以确认驱动识别了 SMMU BBML2 并用于 SVA 兼容性检查，但不能据此推断普通 IOMMU map/unmap 已全面采用免 Break 的拆分、合并流程。

## 10. 与 Dirty Tracking 和热迁移的关系

BBM/BBML 与 Dirty Tracking 解决不同问题，但在迁移期间可能相互关联。

| 机制 | 解决的问题 |
|---|---|
| Dirty Tracking | 哪些 Guest 内存页被修改，需要重新同步 |
| BBM / BBML | 修改地址转换结构时，如何保持架构要求的正确性 |
| TLB 失效 | 如何撤销旧地址、权限或其他缓存转换 |

例如，为了按 4 KiB 粒度跟踪写入，虚拟化软件可能需要先把大粒度 Stage 2 映射拆成小页，再建立写保护或硬件脏状态跟踪条件。

这个过程中：

1. 拆分映射需要满足对应 translation stage 的 BBM/BBML 规则。
2. 建立写保护后，需要完成必要失效，才能依赖旧可写转换已经撤销。
3. 脏状态的获取和清除必须与并发 CPU/设备写入协调。
4. 设备经 SMMU 写入的场景，还必须覆盖对应的转换缓存与脏状态来源。

BBML 可以降低部分页表结构更新的代价，但不会自动记录脏页，也不会替代迁移协议对并发写入和最终状态一致性的处理。

## 11. 常见误解

| 误解 | 正确理解 |
|---|---|
| 原子写 PTE 就不需要 BBM | 原子写不清除其他观察者已有的缓存转换 |
| 持有页表锁就可以直接覆盖所有描述符 | 页表锁不能停止硬件使用旧 TLB 项 |
| BBML2 表示不再需要 TLBI | 只放宽特定粒度变化的更新约束，其他维护仍按场景执行 |
| CPU BBML2 一定不会产生 conflict abort | CPU Level 2 仍可能产生，Level 3 才提供相关额外保证 |
| CPU 与 SMMU 同等级一定完全等价 | 两类架构的级别保证不能只按数字对应 |
| SMMU BBML2 可以更新所有配置结构 | STE、CD 等有独立的更新和失效规则 |
| CMD_SYNC 可以代替页表发布屏障 | 命令完成同步与页表内存可见性处理不同 |
| BBM 完成后所有旧页表都能立即释放 | 还要满足 walker、软件读者及生命周期管理要求 |
| BBML 能自动实现 Dirty Tracking | 它只影响转换结构更新，不记录内存写入 |

## 12. 参考资料

1. [Arm ARM：Using break-before-make when updating translation table entries](https://support.arm.com/documentation/ddi0487/md/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D8-The-AArch64-Virtual-Memory-System-Architecture/-D8-17-TLB-maintenance/-D8-17-1-Using-break-before-make-when-updating-translation-table-entries)
2. [Arm ARM：Support levels for changing table or block size](https://support.arm.com/documentation/ddi0487/md/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D8-The-AArch64-Virtual-Memory-System-Architecture/-D8-17-TLB-maintenance/-D8-17-2-Support-levels-for-changing-table-or-block-size)
3. [Arm AArch64 Memory Management Guide：Updating translations](https://support.arm.com/documentation/101811/0106/TLBs-and-Changing-translations/Updating-translations)
4. [Arm ARM：Cache maintenance requirements due to changing memory region attributes](https://support.arm.com/documentation/ddi0487/md/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D8-The-AArch64-Virtual-Memory-System-Architecture/-D8-18-Caches/-D8-18-3-Cache-maintenance-requirements-due-to-changing-memory-region-attributes)
5. [Linux ARM 内核邮件：CPU BBML 与 SMMU SVA 兼容性讨论](https://lists.infradead.org/pipermail/linux-arm-kernel/2025-February/1002770.html)
6. [Linux 补丁说明：由 BBML2_NOABORT 命名迁移到 BBML3](https://lists.openwall.net/linux-kernel/2026/07/08/1770)

部分较旧版本的 Arm 文档只定义 CPU Level 0～2；引用时需要结合文档版本区分 Level 2 与新增的 Level 3 保证。本文中的 Linux 行为以第 9 节实际核对的源码为依据，不将邮件中的补丁提案直接视为已实现功能。
