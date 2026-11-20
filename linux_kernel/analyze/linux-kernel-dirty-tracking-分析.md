# Linux 脏页跟踪（dirty tracking）机制分析

> 分析日期：2026-10-29。基线：upstream Linux v6.6；PAGEMAP_SCAN 与异步 UFFD-WP 单列 v6.7，IOMMUFD/ARM SMMU 硬件追踪补充核对 v6.12。发行版与 OLK 的回移情况须另行核查。本文为源码与文档分析，未做运行期功能验证。

## 1. 概述：先分清"脏"的几种含义

Linux 没有统一名为 dirty-tracing 的通用子系统；通常所指是 dirty tracking。"dirty" 在不同子系统中有不同的观察对象与语义，不能混用：

- **页缓存 PG_dirty**：文件页缓存需要回写的状态，不是进程自定义时间窗口的写入历史。`folio_mark_dirty()`、文件系统 `dirty_folio` 与 writeback 路径管理它；DIRTY 与 WRITEBACK 又是不同状态。不能用它替代迁移日志。
- **KVM 脏页跟踪**：记录客户机哪些物理页在某个时间段被写过，用于热迁移的增量拷贝。
- **Device Mapper dirty log**：镜像（mirror）目标用来标记磁盘区域在多腿写入期间是否一致，属于 RAID 一致性语义。
- **dm-log-writes**：另一个 DM 目标，逐条记录写操作内容以供回放取证，与上面的一致性 log 是两回事。

本文主线是 KVM 与内存层面的脏页跟踪，其余作为对照。

## 2. 核心不变量

用于增量复制的页级跟踪通常遵循以下逻辑；这里以写保护为例，硬件 dirty bit 方案则通过读取、清位与必要的缓存同步建立下一轮观察窗口：

1. **开纪元（epoch）**：把所有被跟踪页置为只读并清除脏标记，即重新装填写保护。
2. **允许写入**：写者触发 #PF，内核把该页标记为脏。
3. **收割脏集**：从位图或环里取出脏页集合。
4. **拷贝前先重新设防（rearm）**：完成该页的清位、重新跟踪与必要的 TLB 同步，再复制内容；复制期间或之后的新写必须留在下一轮脏集中，不能被本轮清除覆盖。KVM ring 明确要求 reset ioctl 先于读取页内容。
5. **收尾**：最后停掉所有写者并排空（drain）所有尚未收割的脏记录，才能得到一致拷贝。

由此推出几条常被误解的性质：

- **脏只代表"写过"，不代表"内容变了"**。一页被写后又写回相同内容，仍然算脏。
- **没有指令历史**：只知道某页在某纪元被写过，不知道写过几次、写的顺序。
- **日志本身不是全局一致快照**：不同页可在不同时刻采集。要形成一致内存终态，必须协调全部写者、完成日志排空与最终复制；设备状态和 CPU 状态还需另外协调。

一个丢写窗口：页面已 dirty 且可写；复制线程读走旧内容后，另一线程修改它；随后复制线程才清 dirty 并重新设防。后一次修改可能没有进入复制结果，其 dirty 状态又被清掉。正确顺序是先重新设防再复制，使复制期间的写进入下一轮。首次全量复制也需要事先启动跟踪。

## 3. KVM 脏页跟踪：位图与手动日志

KVM 提供两种脏页收集后端，由 `kvm_use_dirty_bitmap()` 决定是否走位图路径：当未启用脏环，或启用了脏环且设置了 `dirty_ring_with_bitmap` 时，仍使用位图。

**启用与收集**：通过 `KVM_SET_USER_MEMORY_REGION` 在 memslot 设置 `KVM_MEM_LOG_DIRTY_PAGES`，再用 `KVM_GET_DIRTY_LOG` 获取每页一位的 bitmap。GFN 为客户机物理页编号；返回 bit 0 对应该 slot 的第一页。默认 GET 会清除本轮状态并重新跟踪；manual 模式除外。

**检测与上报分层**：写保护路径在首次写入时处理故障、标脏并恢复写权限，但不是所有架构都靠此检测。x86 可利用 EPT A/D 与 Intel PML 等硬件能力，v6.6 arm64 KVM 则主要通过 stage-2 写保护跟踪。PML 是硬件收集机制，dirty ring 是内核向用户态上报机制，两者不是同义词。

这种方式实现简单、语义直观，但有两个固有代价：

- **写保护开销与 TLB 失效**：装填写保护要改 PTE 的 W 位并刷 TLB，SMP 上是 shootdown，随后每次首次写都多一次 #PF。纪元切换越频繁，这部分放大越明显。
- **位图内存随内存线性增长**：每 4 KiB 一位。换算到 1 TiB 内存：2^40 / 2^12 = 2^28 页，2^28 位 = 2^25 字节 = 32 MiB 位图。规模上可控，但稀疏写场景下大部分位恒为 0，仍要全量遍历。

**手动日志保护（manual dirty log protect）**：`KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2` 把"读脏日志"与"重新装填写保护"解耦成两步。默认自动模式下，`KVM_GET_DIRTY_LOG` 会顺带清位并重装填；手动模式下用户态可先 `KVM_GET_DIRTY_LOG` 取走脏集，再单独 `KVM_CLEAR_DIRTY_LOG` 指定区间重装填。这对应第 2 节不变量的第 4 步，让用户态能在拷贝前精确控制重装填时机，而不是在读取时一刀切。手动模式不是新机制，只是把重装填的控制权交给用户态。

**能力协商**：通过 `KVM_CHECK_EXTENSION` 探测、`KVM_ENABLE_CAP` 启用可选能力。manual 模式需 `KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE`；可选 `KVM_DIRTY_LOG_INITIALLY_SET` 令初始 bitmap 全脏，便于逐批建立跟踪。ring 需在创建 vCPU 前启用，大小以字节传入且为 2 的幂；仅头文件定义宏不证明运行内核支持。MM 路径同样要检查 `CONFIG_MEM_SOFT_DIRTY`、`CONFIG_USERFAULTFD` 等配置与 `UFFDIO_API` 协商结果。

**大页粒度**：不能从 guest 使用 2 MiB 大页直接推断 KVM 日志粒度也是 2 MiB；guest 页表、宿主后备页和二级页表是三层不同概念。KVM 可拆分二级大页映射来实现细粒度跟踪，代价是 TLB 覆盖下降和页表开销；保持粗粒度的其他追踪路径则可能扩大脏集。

## 4. KVM 脏环（dirty ring）

脏环是为了缓解位图在稀疏写场景下的遍历开销而引入。

环是**每 vcpu 一个**（`vcpu->dirty_ring`），结构定义在 `include/linux/kvm_dirty_ring.h`：`dirty_index` 指向下一个写入槽，`reset_index` 指向下一个待重装填的脏页，`soft_limit` 触发用户态收割。环大小以字节传入，`kvm_dirty_ring_alloc()` 换算成条目数并 `vzalloc` 分配。

**条目状态机**：每个 `kvm_dirty_gfn` 有 `flags` 字段，周期为 **0 → DIRTY → RESET → 0**（见 `virt/kvm/dirty_ring.c`）：

- 初始与无效：`kvm_dirty_gfn_set_invalid()` 用 `smp_store_release(&gfn->flags, 0)`。
- vcpu 写脏压入：`kvm_dirty_gfn_set_dirtied()` 置 `KVM_DIRTY_GFN_F_DIRTY`，压入前用 `smp_wmb()` 保证 slot/offset 先于 flags 可见（内核侧无配对读者，发布给用户态）。
- 用户态消费后标 RESET。
- `KVM_RESET_DIRTY_RINGS` 复位：内核按序处理 RESET 项，代码先归零 entry，再批量调用 `kvm_reset_dirty_gfn()` → 架构 MMU 重新跟踪。外层 `kvm_vm_ioctl_reset_dirty_pages()` 完成必要的远端 TLB 刷新后返回。因此不能把 entry 归零视为已经完成重新设防；必须等待 ioctl 完成。

**重置合并**：同 slot 且 offset 落在 BITS_PER_LONG 范围时可合并 mask，减少架构 MMU 调用与锁操作。TLB 同步在外层批量完成；bitmap 同样有批处理，不能将此视为 ring 独有优势。

**acquire 与 release 语义**：内核先写 slot/offset，再用屏障发布 DIRTY；用户态必须以 acquire 或等效屏障观察 flags，消费后以 release 或等效屏障发布 RESET。内核 acquire 观察 RESET、release 归零。arm64 等弱序平台应探测 `KVM_CAP_DIRTY_LOG_RING_ACQ_REL`，不能只依赖普通 C 读写。消费必须按序，不能跳项。

**背压**：达到 soft limit 后设置 `KVM_REQ_DIRTY_RING_SOFT_FULL`，使 `KVM_RUN` 返回 `KVM_EXIT_DIRTY_RING_FULL`；未及时收割与 reset 就不能继续运行该 vCPU。预留项为 `KVM_DIRTY_RING_RSVD_ENTRIES` 加 `kvm_cpu_dirty_log_size()`，用于容纳包括硬件缓冲排空在内的剩余记录，不是专门给 reset 路径使用。hard full 正常不应到达。

**重装先于拷贝**：`KVM_RESET_DIRTY_RINGS` 的重装（rearm）必须在用户态拷贝页内容之前完成，否则如前述不变量会丢写。收割的语义本身就把"重装"和"清环"绑在一起，重装未完成不应进入拷贝阶段。

**with-bitmap 运行期能力**：检查并启用 `KVM_CAP_DIRTY_LOG_RING_WITH_BITMAP`，用于接住无 vCPU/ring 上下文的内核写入，例如 ARM VGIC/ITS 保存表。该能力依赖先启用 ACQ_REL ring，且必须在建立 memslot 前启用。最终相关内核保存操作完成后再读取备用 bitmap，不能把它当 ring 的完整副本。

**硬件缓冲排空**：用户态看到 ring 暂时为空，不代表 CPU 的硬件日志已全部进入 ring。v6.6 ABI 要求在需要完整结果时让 vCPU 退出 `KVM_RUN`，以排空硬件记录。用户态设备模拟写入、DMA 等其他来源也必须纳入迁移框架自己的脏集。

**稀疏优势并非保证**：ring 避免全量扫描大 bitmap，但每条记录为 16 字节，同一页也可能重复出现，需要合并。高写入率下还会增加退出、收割与 reset 成本；是否更快取决于负载、ring 大小与调度策略，本文无性能实测。

## 5. 软脏位（soft-dirty）

软脏位是 PTE 上一位，用于跟踪任务写过哪些页，典型用法是检查点恢复（CRIU）。流程见 v6.6 soft-dirty.rst：

1. 向 `/proc/PID/clear_refs` 写 `4` 清除软脏位。内核此时同时清掉 PTE 的 W 位。
2. 等一段时间。
3. 读 `/proc/PID/pagemap`，**第 55 位**为软脏位。置位表示自第 1 步以来该 PTE 被写过。

机制本质：清软脏位时一并去掉 W 位，之后写触发 #PF，内核快速处理，页已在内存，只需恢复 W 位并置软脏位。

**共享内存与换出的信息丢失**：共享页被 zap 或换出时 PTE 被清，只存在 PTE 上的 SOFT_DIRTY 随之不可逆丢失。换出页在内核侧仍能从页缓存取回 swap 位置，但 SOFT_DIRTY 取不回。用户态要靠 lseek 的 SEEK_DATA 或 mincore 区分"在内存、已换出、从未分配"，否则会把换出页误判为从未写过的空页。这也是软脏不能当全局真相的又一原因。

**局限**：

- **VMA 更新假阳性**：任务 munmap 后在原地址重映射，旧 PTE 含软脏位被清除；内核为通知这种区域重建，总是把新映射、扩展区域标为软脏。于是"区域重建"会被误报为脏。
- **别名与 DMA 不全覆盖**：软脏只反映经过该进程页表的 CPU 写。同一物理页通过别的映射（别名）写，或设备 DMA 直接写，不会置该 PTE 的软脏位。因此软脏不是"该物理页被写过"的全局真相。

## 6. UFFD 写保护（UFFD-WP）：同步与异步

userfaultfd 的 WP 模式，v6.6 userfaultfd.rst 描述的是**同步**形态：用 `UFFDIO_REGISTER_MODE_WP` 注册区间，`UFFDIO_WRITEPROTECT` 上保护后，写触发 `UFFD_PAGEFAULT_FLAG_WP` 事件。

**同步 WP 阻塞写者**：写者陷入 userfaultfd，**一直阻塞到用户态处理完**（再次 `UFFDIO_WRITEPROTECT` 去保护唤醒）。用户态可在解除前先记账这次写。代价是写者延迟完全取决于用户态响应速度。

**异步 WP（async，v6.7 标注）**：配合 `UFFD_FEATURE_WP_ASYNC` 与 `UFFD_FEATURE_WP_UNPOPULATED`。异步模式的两个要点：

- **不发送 WP 事件**：内核自动处理该 WP fault，不等待用户态处理者解除保护；清除 UFFD-WP 状态后继续写入。
- **不等于无故障或零延迟**：仍需内核处理写保护故障，其他缺页、锁竞争与调度等待也不因此消失。脏信息靠后续扫描获取，见下节 PAGEMAP_SCAN。

**none-pte 行为差异**：WP 在缺页（none pte）上的行为因内存类型而异。匿名内存上 `UFFDIO_WRITEPROTECT` 默认忽略 none pte，即缺页时不投递 WP 故障；文件后内存（shmem、hugetlbfs）上 none pte 也被写保护，写缺页会生成 WP 故障消息。`UFFD_FEATURE_WP_UNPOPULATED` 正是为匿名内存补上这条，让 none pte 也能被写保护。这影响跟踪是否覆盖"从未填充"的区域，迁移前预填充或开 UNPOPULATED 可避免漏跟踪。

## 7. PAGEMAP_SCAN（v6.7 已核对）

PAGEMAP_SCAN 是 pagemap 文件上的 ioctl（见 v6.7 pagemap.rst），支持扫描地址区间、按类别匹配取页，并可同时对这些页上写保护，三者可合成一次原子操作。

**PAGE_IS_WRITTEN 不等于 bit 55**：`PAGE_IS_WRITTEN` 表示"页自上次被写保护后又被写过"，其判定基于 **UFFD-WP 异步状态**，而非 pagemap 第 55 位的软脏位。文档明确把它定位为软脏位的"更高性能替代"，且不受 VMA 合并影响（对普通页能找到真正的软脏页，THP 与 Hugetlb 仍可能有额外脏页报告）。

典型用户态脏跟踪流程（文档给出）：

1. 创建 userfaultfd，设 `UFFD_FEATURE_WP_UNPOPULATED` 加 `UFFD_FEATURE_WP_ASYNC`。
2. `UFFDIO_REGISTER_MODE_WP` 注册区间。
3. 用 `PAGEMAP_SCAN` 的 `PM_SCAN_WP_MATCHING`（或 `UFFDIO_WRITEPROTECT`）上保护。
4. 之后用 `PAGEMAP_SCAN` 查 `PAGE_IS_WRITTEN` 并可顺带上保护，循环收割。

**缓冲区满与续扫**：walk 可能因用户缓冲区满等原因提前结束，UAPI 字段为 `walk_end`，用户态据此续扫；早期文档写作 `end_walk`，应以头文件与实现为准。可用 `PM_SCAN_CHECK_WPASYNC` 检查扫描范围是否满足异步保护要求。

**原子性边界**：扫描与重新保护可合并，避免用户态先读状态、再另行保护的竞态；相关页表项在页表锁保护下检查与修改。但整个范围并非在同一时刻冻结，扫描中也可释放锁或提前结束，所以单次 ioctl 本身也不是整个进程的原子快照。最终一致仍须协调停写与最终复制。

## 8. IOMMU 与 VFIO DMA 脏跟踪

设备 DMA 写内存也会让页变脏，热迁移带直通设备（VFIO）的虚拟机时必须覆盖这部分。这里要区分两种机制：

- **设备脏日志（device dirty logging）**：由设备驱动或设备本身报告它 DMA 写过哪些页，把脏信息回送给迁移框架。这是"设备主动上报"。
- **IOMMU 脏跟踪（IOMMU dirty page tracking）**：由 IOMMU 硬件跟踪哪些 IOVA 页被设备写入，是"IOMMU 硬件被动跟踪"。

二者记录来源不同，但用户接口均可按 IOVA 范围报告 bitmap，不能用 HPA 与 IOVA 的区别来区分两者。迁移框架需按映射关系转换、合并到 Guest RAM 脏集；CPU PTE 写保护一般不能捕获 DMA。

**v6.6 已有 VFIO 接口**：设备侧通过 `VFIO_DEVICE_FEATURE_DMA_LOGGING_START/STOP/REPORT` 启停及读取日志，需按设备探测支持。传统 Type1 的 `VFIO_IOMMU_DIRTY_PAGES` 也有 START/STOP/GET_BITMAP，但记录可能是保守的 potentially dirty 集合，并不证明 IOMMU 有精确硬件 dirty bit。不能只看 ioctl 名称判断精度。

**v6.6 与后续版本边界**：v6.6 IOMMUFD 文档仍将硬件 dirty tracking 列为 TODO。已核对的 v6.12 则具有 `IOMMU_HW_CAP_DIRTY_TRACKING`、`IOMMU_HWPT_SET_DIRTY_TRACKING` 和 `IOMMU_HWPT_GET_DIRTY_BITMAP`；后者默认读取并清除 dirty 元数据，`NO_CLEAR` 只读。必须检查具体硬件、驱动和 domain 能力，不能从版本号或 IOMMU 存在推断支持。

**ARM SMMU v6.12 实例**：调用链为 IOMMUFD/HWPT → `iommu_dirty_ops` → `arm_smmu_read_and_clear_dirty()` → `io_pgtable_ops.read_and_clear_dirty()` → `arm_lpae_read_and_clear_dirty()`。这里核对的是 ARM_64_LPAE_S1 路径，不是所有 SMMU stage 都通用。

`IO_PGTABLE_QUIRK_ARM_HD` 下，DBM=1 与 AP[2] 组合表示可写映射的 dirty 状态：AP[2]=0 为 writable-dirty；读取后通过原子置 AP[2]=1 恢复 writable-clean，后续由硬件脏状态更新处理写入。`io_pgtable_visit_dirty()` 按叶映射范围记录 bitmap，block 映射可放大脏粒度。故硬件追踪也不是只读一个独立 dirty 位；权限修改、硬件更新并发与映射粒度都必须审查。

## 9. Device Mapper 的两种"脏 log"

- **dm-log（dirty log）**：device-mapper RAID（如 mirror）用它标记磁盘区域的**一致性**。一个区域在多腿写入进行中算 dirty 或 inconsistent（写要复制到所有 mirror leg，可能不同时到达），全部写完算 clean。实现分 `disk`（状态落盘，重启崩溃后存活）、`core`（状态在内存，不落盘，略快）、`userspace`（转发到用户态守护进程，含 clustered 变体）。它跟踪的是"mirror 腿之间是否一致"，不是写内容、也不是迁移脏页。
- **dm-log-writes**：另一个 DM 目标，逐条**记录写操作的内容**以供后续回放与取证。与 dm-log 的"一致性 log"是完全不同的特性，只是名字都带 log 与 writes。

## 10. 机制对比矩阵

| 维度 | KVM 位图 | KVM 脏环 | soft-dirty | UFFD-WP 同步 | UFFD-WP 异步加 PAGEMAP_SCAN(v6.7) | IOMMU 跟踪(v6.6) | dm-log |
|---|---|---|---|---|---|---|---|
| 跟踪对象 | guest GFN | guest GFN | 进程 VA | 进程 VA | 进程 VA | 设备 DMA IOVA | 磁盘区域一致性 |
| 数据结构 | memslot 位图 | 每 vcpu 环 | PTE 状态，pagemap bit55 导出 | uffd 事件 | UFFD-WP 状态 | v6.6 IOMMUFD 尚无该 UAPI | 区域状态表 |
| 写者是否等待用户态 | 通常不等待 | ring 满会退出 | 不等待 | WP fault 等待处理者 | 不等待 WP 事件处理者 | N/A | N/A |
| 稀疏写表现 | 全量扫位图 | 只记脏页 | 扫 pagemap | 逐写事件 | 轮询 PAGE_IS_WRITTEN | N/A | N/A |
| 收集限制 | 扫描成本 | KVM_EXIT_DIRTY_RING_FULL | 扫描成本 | 故障处理延迟 | 缓冲满 walk_end 续扫，非写者背压 | N/A | N/A |
| 设防与复制 | GET；manual 为 CLEAR 后复制 | RESET ioctl 完成后复制 | clear_refs=4 后复制 | 可保持 WP 完成复制后再放写 | 扫描并 WP 后复制 | N/A | N/A |
| 全局一致快照 | 否 | 否 | 否 | 否 | 否 | N/A | 仅 mirror 一致性 |

## 11. 适用场景速查

不同机制对应不同需求，选错会把开销或漏跟踪带进系统：

- **KVM 热迁移 precopy**：首选 KVM 位图或脏环。内存大且写稀疏时脏环减少全量扫描；写密集时位图的批量置位更稳。
- **KVM 热迁移 postcopy**：用 userfaultfd 的 MISSING 模式做按需拉页，WP 不是主路径，但 precopy 一轮先迁移只读区可减少 postcopy 期 userfault。
- **进程检查点恢复（CRIU）**：软脏位是经典选择，成本在 clear_refs 与读 pagemap。若能接受 UFFD-WP 异步加 PAGEMAP_SCAN，可避开 VMA 合并假阳性。
- **不希望写故障等待用户态处理者**：异步 UFFD-WP 加 PAGEMAP_SCAN（v6.7），靠轮询收割；仍会有内核故障处理与调度延迟，并非实时无阻塞保证。
- **用户态脏跟踪且可接受写延迟**：同步 UFFD-WP，逐写事件记账，简单但写延迟受用户态响应制约。
- **带直通设备的迁移**：必须覆盖设备 DMA 脏来源。v6.6 应核对 VFIO 设备日志或 Type1 保守跟踪能力；采用后续 IOMMUFD 硬件方案则需检查实际支持。

## 12. 局限性汇总

- KVM/MM 脏页集合不提供指令级写历史；soft-dirty 等还可能包含映射变更带来的保守报告。dm-log-writes 则是另外的写内容日志。
- 所有机制都不天然提供全局一致快照，需靠"停写加排空"收尾。
- 软脏与 UFFD-WP 都只覆盖经过页表的 CPU 写；DMA、别名映射不自动覆盖。
- 大页会放大脏粒度，迁移与拷贝流量随之增加。
- 位图内存线性增长（1 TiB / 4 KiB 约等于 32 MiB），大规模内存下不可忽略。
- 脏环稀疏优势有条件，高写入率下背压退出可能抵消收益。
- IOMMU 硬件脏跟踪在 v6.6 IOMMUFD 中为 TODO，带直通设备的迁移需另行核对。

## 13. 源码阅读地图

按主线顺序阅读效率较高：

1. KVM 位图：`Documentation/virt/kvm/api.rst`（v6.6）的 `KVM_GET_DIRTY_LOG`、`KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2` 等条目。
2. KVM 脏环核心：`virt/kvm/dirty_ring.c`（v6.6）看 `kvm_dirty_ring_push`、`kvm_dirty_ring_reset`、`kvm_dirty_ring_check_request`、`kvm_reset_dirty_gfn`；`include/linux/kvm_dirty_ring.h`（v6.6）看结构与 `kvm_use_dirty_bitmap`。
3. 软脏位：`Documentation/admin-guide/mm/soft-dirty.rst`（v6.6）。
4. UFFD-WP：`Documentation/admin-guide/mm/userfaultfd.rst`（v6.6）的 Write Protect Notifications 与 QEMU/KVM 段。
5. PAGEMAP_SCAN 与异步 WP：`Documentation/admin-guide/mm/pagemap.rst`（**v6.7**）的 Pagemap Scan IOCTL 段。
6. IOMMUFD 现状与 TODO：`Documentation/userspace-api/iommufd.rst`（v6.6）的 Future TODOs。
7. dm-log：`Documentation/admin-guide/device-mapper/dm-log.rst`（v6.6）；dm-log-writes：`https://docs.kernel.org/admin-guide/device-mapper/log-writes.html`。

## 14. 说明

本文基于版本固定的上游源码与文档，未做编译、QEMU 或性能验证；没有修改内核代码。以下链接供复核，不代表已核对所有后续版本与发行版回移。

### 参考来源

- [v6.6 KVM API：GET/CLEAR、manual protect、ring 及备用 bitmap](https://github.com/torvalds/linux/blob/v6.6/Documentation/virt/kvm/api.rst)
- [v6.6 dirty_ring.c：状态、发布、复位与背压](https://github.com/torvalds/linux/blob/v6.6/virt/kvm/dirty_ring.c)
- [v6.6 kvm_main.c：mark_page_dirty_in_slot 与 reset ioctl/TLB 同步](https://github.com/torvalds/linux/blob/v6.6/virt/kvm/kvm_main.c)
- [v6.6 arm64 KVM MMU](https://github.com/torvalds/linux/blob/v6.6/arch/arm64/kvm/mmu.c)；[x86 MMU](https://github.com/torvalds/linux/blob/v6.6/arch/x86/kvm/mmu/mmu.c)
- [v6.6 soft-dirty 文档](https://github.com/torvalds/linux/blob/v6.6/Documentation/admin-guide/mm/soft-dirty.rst)
- [v6.6 userfaultfd 文档](https://github.com/torvalds/linux/blob/v6.6/Documentation/admin-guide/mm/userfaultfd.rst)；[v6.6 UAPI，无 WP_ASYNC](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/userfaultfd.h)
- [v6.7 pagemap 文档](https://github.com/torvalds/linux/blob/v6.7/Documentation/admin-guide/mm/pagemap.rst)；[task_mmu.c：PAGE_IS_WRITTEN 与 walk_end](https://github.com/torvalds/linux/blob/v6.7/fs/proc/task_mmu.c)
- [v6.6 VFIO UAPI：设备 DMA logging 与 Type1 dirty pages](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/vfio.h)
- [v6.6 IOMMUFD TODO](https://github.com/torvalds/linux/blob/v6.6/Documentation/userspace-api/iommufd.rst)；[v6.12 IOMMUFD UAPI](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/iommufd.h)
- [v6.12 ARM SMMUv3 dirty_ops](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c)；[io-pgtable-arm：DBM/AP 与读取清除](https://github.com/torvalds/linux/blob/v6.12/drivers/iommu/io-pgtable-arm.c)
- [v6.6 dm-log 文档](https://github.com/torvalds/linux/blob/v6.6/Documentation/admin-guide/device-mapper/dm-log.rst)；[dm-log-writes 文档](https://docs.kernel.org/admin-guide/device-mapper/log-writes.html)
