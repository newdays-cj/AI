# page_owner 功能问题分析

## 1. 分析基线与结论

- 分析日期：2026-11-05。
- 源码目录：`/home/chenjun/Git/linux`。
- 源码版本：根目录 `Makefile` 标识为 Linux `7.3-rc7`。本次未读取 Git 提交号，不能据此定位到唯一提交。
- 本地配置：`CONFIG_64BIT=y`、`CONFIG_PAGE_SHIFT=12`、`CONFIG_SPARSEMEM_VMEMMAP=y`、`CONFIG_PAGE_OWNER=y`。
- 范围：page_owner 核心、分配/释放、迁移/拆分、page_ext 生命周期、debugfs 接口、用户态工具和 GDB 辅助脚本。
- 方法：源码调用路径分析、用户态工具编译、合成输入、ASan/UBSan 与输出行为验证。
- 验证边界：没有修改内核源码，没有启动测试内核，没有进行内核热插拔、迁移或 32 位运行验证；GDB 脚本未在 vmcore 上验证。

主要问题集中在三个方面：用户态排序器存在已经复现的越界读取，内核栈汇总可能永久漏项，迁移相关历史信息存在失真。另外还存在排序配置失效、错误返回成功、读接口边界、并发过滤、跨架构兼容和 GDB 脚本问题。

本文区分已动态复现的问题、由源码路径确认的问题、尚待验证的风险与功能本身的限制。覆盖上述路径不等于证明所有潜在缺陷已经穷尽。文中的源码行号以分析时工作区为准，后续修改可能使其偏移。

## 2. 功能结构与关键路径

| 模块 | 主要位置 | 职责 |
| --- | --- | --- |
| 核心实现 | `mm/page_owner.c` | 保存分配/释放信息、计数、迁移与拆分处理、debugfs 输出 |
| 内核接口 | `include/linux/page_owner.h` | 静态分支包装及配置关闭时的空实现 |
| 配置与构建 | `mm/Kconfig.debug:105-120`、`mm/Makefile:117` | PAGE_OWNER 依赖、选项选择和对象构建 |
| 扩展存储 | `mm/page_ext.c`、`include/linux/page_ext.h` | 每页扩展布局、RCU 生命周期及热插拔处理 |
| 栈存储 | `lib/stackdepot.c`、`include/linux/stackdepot.h` | 调用栈去重、存储和记录计数 |
| 分配与释放 | `mm/page_alloc.c` | 调用 `set_page_owner()`、`reset_page_owner()` 和拆分接口 |
| 迁移与拆分 | `mm/migrate.c`、`mm/huge_memory.c`、`mm/hugetlb.c` | 所有权信息转移、迁移原因与 order 调整 |
| 分析工具 | `tools/mm/page_owner_sort.c`、`tools/mm/page_owner_filter.c` | 用户态归并、排序和内核过滤配置 |
| 调试脚本 | `scripts/gdb/linux/page_owner.py` | GDB 中逐页或全量读取 owner 信息 |
| 使用文档 | `Documentation/mm/page_owner.rst` | 接口、工具参数和使用限制 |

`page_owner=on` 启动参数请求启用；初始化时创建特殊栈记录、补记早期已分配页面，并启用 `page_owner_inited` 静态分支。分配时记录 order、GFP、调用栈、任务和时间；释放时记录释放信息并扣减栈的基础页计数。拆分调整各基础页的 order；迁移复制分配信息并交换用于计数的 handle。

主要输出接口包括 `/sys/kernel/debug/page_owner`，以及 `page_owner_stacks/` 下的 `show_stacks`、`show_handles`、`show_stacks_handles` 和 `count_threshold`。

## 3. 确定问题

### PO-01：排序器在常规解析路径中越界读取字符串

- 严重度：高，影响用户态工具。
- 证据等级：ASan 已复现。
- 位置：`tools/mm/page_owner_sort.c:230`、`:530-540`。

工具为记录分配 `len + 1` 字节，复制 `len` 字节后，没有立即补上字符串结尾的 NUL，便调用 `remove_pattern()`。后者把缓冲区传给要求 NUL 结尾字符串的 `regexec()`，可能读出堆分配边界。终止符直到第 540 行才写入。

包含正常首行、PFN 行和调用栈的合成记录触发了以下报告：

```text
ERROR: AddressSanitizer: heap-buffer-overflow
#0 regexec
#1 remove_pattern tools/mm/page_owner_sort.c:230
#2 add_list tools/mm/page_owner_sort.c:538
```

同一函数在第 234 行使用 `memcpy()` 搬移字符串内重叠的区域，也违反其调用契约。修复需要同时处理调用正则前的字符串终止和重叠区域搬移。

该结果证明用户态工具存在内存访问错误，不代表内核发生越界。

### PO-02：VMALLOC 识别向缓冲区起始地址之前读取

- 严重度：中。
- 证据等级：ASan 已复现；当前内核正常输出的可达性未证明。
- 位置：`tools/mm/page_owner_sort.c:450-457`。

`get_allocator()` 找到 `__vmalloc_node_range` 后，向前寻找两次换行，没有检查是否到达 `buf`。当该符号出现在记录的第一条栈帧时，第二次向前搜索会越过首行，读到缓冲区之前。

用该布局的合成记录，并以 `-a` 避开 PO-01 后，得到：

```text
ERROR: AddressSanitizer: heap-buffer-overflow
get_allocator tools/mm/page_owner_sort.c:456
... 1 bytes before 131072-byte region
```

这证明解析器无法安全处理该布局。修复应对向前搜索设置记录起始边界，或使用受边界约束的逐行解析。

### PO-03：栈记录首次登记失败后永久缺席汇总接口

- 严重度：中。
- 证据等级：源码路径确认，未做内核故障注入。
- 位置：`mm/page_owner.c:192-245`。

首次使用某个栈记录时，代码先将计数从 `REFCOUNT_SATURATED` 改为 1，再调用 `add_stack_record_to_list()`。后者在 GFP 标志不允许 spinning 或分配 `struct stack` 失败时直接返回。

此时计数已初始化，链表登记却未完成。以后同一栈被使用时不会再进入首次登记分支，也没有补登机制。该栈将永久缺席 `show_stacks`、`show_handles` 和 `show_stacks_handles`。

逐页 owner handle 仍可能存在，因此逐页统计和汇总接口会不一致。受限分配路径及内存压力是相关触发条件。

修复需要让计数初始化与链表登记形成一致的状态转换，或提供可靠补登机制。不能未经并发分析便在失败时简单回写计数。

### PO-04：迁移后的释放历史字段来源不一致

- 严重度：中。
- 证据等级：源码路径确认。
- 位置：`mm/page_owner.c:291-312`、`:426-429`。

`__update_page_owner_free_handle()` 接受 `pid`、`tgid` 参数，却实际写入 `current->pid`、`current->tgid`。普通释放时两者一致；迁移调用传入旧页面的释放身份时，该身份却被替换为迁移线程。

此外，迁移调用传入 `handle == 0`，不会复制旧页面的 `free_handle`，但会复制其 `free_ts_nsec`。目的 PFN 可能保留自身先前的释放栈。

最终可能出现释放栈来自目的 PFN、释放时间来自源 PFN、释放 PID/TGID 来自迁移线程的混合记录，误导页面历史分析。

修复前需要明确释放历史应跟随物理 PFN 还是逻辑分配，再一致处理 handle、时间和任务身份。

### PO-05：大 folio 迁移后再拆分，子页丢失最近迁移原因

- 严重度：中。
- 证据等级：源码路径确认，未做内核运行验证。
- 位置：`mm/page_owner.c:366-390`、`mm/migrate.c:1423`、`mm/huge_memory.c:3886`。

`__folio_set_owner_migrate_reason()` 只更新 folio 头页。之后 `__split_page_owner()` 只修改各基础页的 order，不传播头页的迁移原因。

触发顺序为大 folio 分配、成功迁移、更新头页迁移原因、拆分。由原尾页形成的新 folio 头页仍保存旧原因，甚至是 `MR_NEVER`，与真实迁移历史不一致。

可考虑在迁移原因更新时覆盖全部相关基础页，或在拆分时把一致的 folio 级信息传播到新头页，具体方案需结合并发契约确定。

### PO-06：`--sort` 配置被后续默认设置覆盖

- 严重度：中。
- 证据等级：行为已复现。
- 位置：`tools/mm/page_owner_sort.c:598-645`、`:810-835`。

`parse_sort_args()` 建立比较器后，参数解析结束处的 `switch (compare_flag)` 又调用 `set_single_cmp()`。只使用 `--sort` 时，`compare_flag` 仍为 `COMP_NO_FLAG`，自定义排序被默认次数排序覆盖。

输入 PID 为 9 和 2 的两条记录，使用 `--sort=-pid`，实际输出仍为 2 在前、9 在后。多字段排序也不能按配置生效。

修复方向是让明确指定的排序配置与默认配置具有清晰优先级，避免默认分支无条件覆盖用户设置。

### PO-07：排序器失败时返回成功，输出错误未反馈

- 严重度：中。
- 证据等级：容量错误和 `/dev/full` 输出错误已验证，其余基于源码。
- 位置：`tools/mm/page_owner_sort.c:856-873`、`:905-951`。

内存分配、记录解析或容量不足后可能进入清理路径，最终仍返回 0；`fstat()` 和 `fprintf()` 的错误未检查；输出文件未显式关闭并检查，退出时隐式刷新错误也无法反馈。

本次输出到 `/dev/full` 时，工具仍以 0 退出。触发容量不足时虽然打印 `max_size too small??`，退出状态也为 0。

这会让自动化流程把缺失或截断的报告当作成功产物。修复需要传播失败状态并检查读取、写入、刷新和关闭阶段的错误。

### PO-08：order 检查不能保证移位合法，累计计数存在溢出边界

- 严重度：中。
- 证据等级：非法移位由 UBSan 复现；累计溢出基于类型和运算路径。
- 位置：`tools/mm/page_owner_sort.c:35-36`、`:311-327`、`:896-897`。

`get_page_num()` 允许 order 到 64，却执行 `1 << order_val`，左操作数是 `int`。输入 `order 32` 时，UBSan 报告：

```text
runtime error: shift exponent 32 is too large for 32-bit type 'int'
```

这是异常输入可触发的问题，不代表正常内核分配会产生 order 32。解析时还应避免在检查范围前将 `strtol()` 结果缩窄为 `int`。

累计次数和页数使用 `int`，足够大的归并结果会溢出；页数比较器也使用减法。4 KiB 页下，约 8 TiB 的页数接近有符号 32 位计数边界。

修复方向是使用足够宽的计数类型、验证转换及移位范围，并使用不会因相减而溢出的比较方式。

### PO-09：逐页读接口的零长度、小缓冲区和单页上限处理不合理

- 严重度：中低。
- 证据等级：源码分析；超长符号记录未做测试内核验证。
- 位置：`mm/page_owner.c:612-670`、`:728-829`。

零长度读取没有提前返回，可能扫描内存后返回 `-ENOMEM`。用户缓冲区放不下完整记录时也返回 `-ENOMEM`，不支持分段读取，错误码无法区分真实分配失败与缓冲区不足。

内部还将缓冲区上限固定为 `PAGE_SIZE`。如果符号名、模块名等导致完整记录超过一页，增大用户缓冲区也无效，该记录在 stack 模式下仍无法读出。其可达性依赖实际符号长度。

修复应明确记录式读取契约，处理零长度读取，并消除无法通过增大缓冲区解决的记录长度限制。

### PO-10：共享 open 的过滤状态缺少并发同步

- 严重度：中低。
- 证据等级：源码调用契约确认，未进行并发运行测试。
- 位置：`mm/page_owner.c:808-820`、`:969-1012`。

写接口先读取旧状态，再依次提交 mode、nodemask 和 enabled 标志，没有专门同步。对同一个 open file description 并发 `pwrite()`，两个更新可能都基于旧状态并互相覆盖。

并发 `pread()`/`pwrite()` 还可能观察到提交中途的状态，尤其是跨多个机器字的 nodemask。普通共享 `read/write` 的位置锁不能替代 positioned I/O 路径上的状态同步。

不同 `open()` 的状态独立，不受此问题影响。目前证据支持过滤结果不确定，不支持据此宣称越界或崩溃。

### PO-11：非法 seek 改变位置，32 位位置和阈值会截断

- 严重度：低。
- 证据等级：源码路径确认，32 位行为未运行验证。
- 位置：`mm/page_owner.c:731-745`、`:838-850`、`:1139-1142`。

`lseek_page_owner()` 直接写入负 offset，也不检查 `SEEK_CUR` 加法溢出。失败的 seek 可能留下无效 `f_pos`，使后续读取失败，直到重新定位。

32 位内核另有两个类型问题：64 位 `loff_t` 被赋给 `unsigned long pfn`，大 offset 会截断并映射回低 PFN；阈值 setter 接受 `u64`，存储却为 `unsigned long`，写入 `4294967296` 可能成功返回但实际阈值为 0。

修复应在提交位置前验证范围，并明确各接口的大数值处理方式。

### PO-12：GDB 脚本与当前数据结构和遍历逻辑不一致

- 严重度：中，影响辅助调试工具。
- 证据等级：静态代码确认，未执行 GDB/vmcore 验证。
- 位置：`scripts/gdb/linux/page_owner.py:132`、`:149-160`、`:180`。

当前 `page->flags` 为包含 `.f` 的 `memdesc_flags_t` 结构，脚本仍直接以 `%x` 输出 `page['flags']`，见 `include/linux/mm_types.h:39-41`。

全量扫描使用 `self.p_ops.MAX_ORDER_NR_PAGES`，但 x86 的 `x86_page_ops` 未定义该属性，见 `scripts/gdb/linux/mm.py:34-92`。执行到需要该属性的路径会失败。

跳过无效范围时使用 `pfn += MAX_ORDER_NR_PAGES - 1` 后 `continue`。Python `while` 没有 C `for` 循环末尾隐含的递增，因此停在无效范围最后一个 PFN，而非下一个边界。

修复应同步字段类型、架构属性和步进逻辑，并以真实 vmcore 覆盖有空洞的内存布局。

### PO-13：不完整输入被静默丢弃，容量估算不可靠

- 严重度：低到中。
- 证据等级：读取路径基于源码，精简输入容量错误已复现。
- 位置：`tools/mm/page_owner_sort.c:113-128`、`:857`、`:867-871`。

`read_block()` 将 EOF、读取错误和缓冲区不足统一返回为 -1，主循环都当作正常结束。最后一条记录缺少末尾空行时会丢失；超过 128 KiB 的记录会终止后续处理；输入 I/O 错误无法与正常 EOF 区分。

当前内核输出有末尾空行，单条记录也受一页限制，因此超长记录和缺少空行主要影响截断、编辑或外部生成的数据。

工具按文件字节数除以 100 估计最大记录数，对精简输入可能不足。本次用不足 100 字节且符合字段解析规则的记录触发了容量错误；该输入不等同于一条完整的当前内核原始输出。

修复应区分结束与错误，明确不完整记录处理策略，并使用可增长的记录容器。

### PO-14：阈值并发访问标注不完整

- 严重度：低。
- 证据等级：静态代码确认，未运行 KCSAN。
- 位置：`mm/page_owner.c:1082`、`:1133-1142`。

setter 使用 `WRITE_ONCE()`，getter 使用 `READ_ONCE()`，但 `stack_print()` 普通读取 `pages_threshold`，缺少对应访问标注。

本机字宽类型不意味着必然发生硬件撕裂；这里的问题是并发访问缺少正式的单次读取约束。可在打印路径读取一次带标注的局部阈值。

### PO-15：文档列出当前工具不支持的释放信息选项

- 严重度：低。
- 证据等级：文档与实现交叉核对；`--cull=free` 拒绝行为已验证。
- 位置：`Documentation/mm/page_owner.rst:202-203`、`:234`、`:265`、`:275`。

文档仍列出 `-r`、`-f`、`free_ts` 和 `--cull=free`，当前工具 getopt 和参数类型解析没有对应实现。当前正常逐页 debugfs 输出也不提供这些选项所需的完整释放信息。

应同步文档与实现，避免用户按文档配置失败，或误认为输出包含释放时间分析能力。

## 4. 待验证的生命周期风险

### 4.1 逐页扫描与打印缺少热移除稳定性保护

`read_page_owner()` 在 `mm/page_owner.c:764-773` 验证 PFN 后，没有取得 online-memory 保护便访问 `struct page` 和 buddy 状态。PFN 检查不是持续的内存生命周期保护。

在 `mm/page_owner.c:826-829` 复制 owner 并释放 page_ext 的 RCU 保护后，打印函数可能进行 `GFP_KERNEL` 分配并睡眠，然后继续访问页面、pageblock 和 memcg 信息，见 `mm/page_owner.c:612-656`。

并发 offline/remove 可能使先前验证失效。应在支持热移除的测试内核上，覆盖扫描、打印分配期间的抢占及实际 memmap 移除窗口，再确认可达后果与合适的锁范围。单纯把 page_ext 的 RCU 临界区扩大到可能睡眠的打印路径并非正确修复。

### 4.2 迁移函数在 page_ext_put 后继续使用 owner 指针

`__folio_copy_owner()` 在 `mm/page_owner.c:401-436` 释放 page_ext 保护后继续访问源和目的 owner 指针。`mm/page_ext.c:556-570` 的契约说明 put 后扩展信息可能不再有效。

不过，内存下线必须先完成迁移、隔离检查，才会释放相关 page_ext，见 `mm/memory_hotplug.c:2051-2152`。分配中 folio 的生命周期和下线顺序也会限制触发条件。

因此不能仅凭 put 后使用就确认当前调用路径存在可达 UAF。本项保留为生命周期核查事项，未计为已确认的高危内核漏洞。可考虑在受保护期间复制全部必要字段到局部存储，同时继续核对实际调用约束。

## 5. 已排除的疑似问题

- 普通 `read()` 返回 `-ENOMEM` 或 `-EFAULT` 不会自动跳过该 PFN。`ksys_read()` 使用局部位置，只有非负返回才提交到 `file->f_pos`，见 `fs/read_write.c:711-718`。
- 栈汇总的 `ctx->stack` 缓存和 `-1UL` EOF 哨兵与 seq_file 调用顺序相容，没有确认此前怀疑的重启、seek 或 32/64 位哨兵比较错误。
- stack list 只前插、不删除，release 发布与 acquire 读取配对；不能把其无锁遍历直接判为悬空指针问题。
- 释放侧 `save_stack(__GFP_NOWARN)` 是有意禁止 spinning 和 pool 补充分配的路径；stack depot 使用 trylock，不应把它直接判为受限上下文中的阻塞锁错误。
- 拆分仅改变 order、按基础页维护计数的总体方案没有被证明破坏计数守恒；PO-05 针对迁移原因元数据，而非基础页计数。
- `page_owner_filter` 短写后继续读取看似可疑，但当前内核写接口成功时返回完整 count，失败时返回负错误；本次未证明正常接口会产生短写，不将其列为确定缺陷。
- NUMA 数字校验最多接受六位数，不能用 `999999` 证明 32 位 int 溢出。节点范围还由内核验证，不将该例当作整数溢出漏洞。

## 6. 功能限制与成本

### 6.1 每页元数据占用明显

当前 `struct page_owner` 为 64 字节。在常见 64 位、4 KiB 页、共享 flags 为 8 字节的布局下，相关 page_ext 约为每页 72 字节，即被覆盖内存的约 1.76%。1 TiB 内存对应约 18 GiB，尚未计入 stack depot、链表节点及其他扩展客户端开销。实际占用应以运行时 page_ext 布局和覆盖范围为准。

### 6.2 历史栈不会随页面释放立即回收

page_owner 使用持久化 stack-depot 记录，页面释放后对应历史栈和汇总节点不会随之释放。长时间运行、调用栈种类多时，调试元数据会积累；其增长主要取决于不同调用栈数量，不能说每次分配都会无条件新增一份栈。

### 6.3 采集会扰动被诊断系统

栈回溯、哈希查找、计数和元数据分配都会增加分配/释放路径成本。受限上下文、锁竞争或资源不足时可能退化为 dummy/failure 栈，降低归因精度。

### 6.4 输出不是一致性快照

扫描不阻止并发分配、释放、迁移或拆分。记录可能漏看，多个字段也可能来自不同时间，不能将一次扫描合计视为精确的时点账本。

### 6.5 早期分配无法恢复真实现场

初始化扫描只能补记页面已占用，不能恢复其原始调用栈、任务和分配时间。补记时间不是实际分配时间，栈汇总也不能完整代表这部分占用。

### 6.6 页面分配来源不等于最终业务责任

page_owner 不直接提供 slab 对象级泄漏归因，也不能仅凭分配任务确定共享页最终使用者或业务请求的内存责任。相关问题需要结合 slab、memcg、对象生命周期或其他观测手段。

### 6.7 时间戳不保证跨 CPU 严格可比较

记录使用 `local_clock()`。不稳定调度时钟平台上跨 CPU 时间可能倒退，不能无条件用分配/释放时间差计算严格寿命。相关契约见 `include/linux/sched/clock.h:86-94`。

## 7. 动态验证记录

验证产物位于本次会话的 `/tmp/opencode/` 临时目录，不属于本文目录内的持久交付件，临时目录清理后需要重新准备输入。

编译命令在源码根目录执行：

```sh
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    tools/mm/page_owner_sort.c \
    -o /tmp/opencode/page_owner_sort_review_20261105
gcc -O2 tools/mm/page_owner_sort.c \
    -o /tmp/opencode/page_owner_sort_review_plain_20261105
```

| 验证项 | 输入或操作 | 实际结果 |
| --- | --- | --- |
| PO-01 | 含正常字段和栈的记录，默认模式，ASan | `regexec()` 路径堆越界读取 |
| PO-02 | 第一条栈帧为 `__vmalloc_node_range`，`-a`，ASan | `get_allocator()` 向分配区之前读取 |
| PO-06 | PID 9 和 2 两条记录，`--sort=-pid` | 实际输出 PID 2、9，自定义排序失效 |
| PO-07 | 输出写入 `/dev/full` | 工具退出状态为 0 |
| PO-07、PO-13 | 不足 100 字节的精简记录 | 打印容量不足错误，退出状态为 0 |
| PO-08 | `order 32`，`-a`，UBSan | 报告 32 位 int 移位指数过大 |
| PO-15 | `--cull=free` | 参数被拒绝 |

其中正常字段示例为：

```text
Page allocated via order 0, mask 0x0(), pid 9, tgid 9 (nine), ts 9 ns
PFN 0x9 type Unmovable Block 0 type Unmovable Flags 0x0()
 example_alloc+0x1/0x2

```

工具按空行划分记录，复现时必须确保文件在最后一条记录后仍有分隔空行，否则会先触发 PO-13。ASan 运行使用了 `ASAN_OPTIONS=detect_leaks=0`；本次结果证明的是列出的越界访问，没有完成工具内存泄漏检查。

## 8. 修复优先级建议

1. 优先修复 PO-01、PO-02 的解析器越界，以及 PO-07 的错误成功返回，确保分析工具安全且结果可被自动化流程信任。
2. 修复 PO-03 的栈汇总永久漏项和 PO-06 的排序配置失效，消除占用归因与排序结果偏差。
3. 明确迁移历史字段语义，修复 PO-04、PO-05，并覆盖大 folio 迁移后拆分的测试场景。
4. 在测试内核上验证热插拔并发访问，确认可达后果后决定 online-memory 与 RCU 的保护范围。
5. 完成读接口、共享过滤状态、32 位边界、GDB 脚本、输入错误处理和文档同步。

以上为分析和修复方向，不表示已实施修复，也不表示已完成内核运行验证。
