# LPC 2026 技术摘要：延迟 SWIOTLB Bounce，缓解 CoCo VM 高带宽读时的软锁死

## 报告信息

- **会议与场次：** Linux Plumbers Conference 2026，Confidential Computing 微会议。
- **讲者与单位：** Ryan Afranji（Google）。
- **时间与地点：** 2026-10-07 17:50，Europe/Prague，Club E（20 分钟）。
- **官方页面标题：** "Deferring SWIOTLB Bounce to Reduce Heavy CPU Utilization"。
- **PDF 封面标题：** "CoCo VM's Soft Lockups During High-Bandwidth Storage Reads"（两者指向同一场报告，封面侧重问题症状，页面标题侧重方案）。
- **官方页面：** [LPC 2026 contribution 2475](https://lpc.events/event/20/contributions/2475/)。
- **官方 PDF：** [06-LPC2026-SWIOTLB-bounce-deferring.pdf](https://lpc.events/event/20/contributions/2475/attachments/2272/4966/06-LPC2026-SWIOTLB-bounce-deferring.pdf)（共 19 页；本文页码均指该 PDF 页码）。
- **RFC 补丁：** [lore.kernel.org 邮件](https://lore.kernel.org/all/20261006143329.2415252-1-afranji@google.com/)（2026-10-06 发出，[RFC PATCH]，单 patch）。
- **SWIOTLB 背景文档：** [kernel.org swiotlb](https://docs.kernel.org/core-api/swiotlb.html)。

## 核心结论

在报告所讨论的强制启用 SWIOTLB bounce 的机密虚拟机（CoCo VM）中，NVMe 读完成阶段的 CPU 拷贝发生在硬中断上下文，高带宽读可占满中断亲和性绑定的 vCPU，触发 soft lockup。提案把整个 completion batch 排到非绑定的高优先级 workqueue，由 worker 线程执行原有 unmap 与请求完成路径（含 bounce 拷贝），将重拷贝移出硬中断。作者称 40 次连续测试中未再观察到 soft lockup；两组基准也显示 IOPS 与带宽提升，但部分延迟分位回退。材料显示这是公开 RFC 原型，未提供已合入主线的依据，启用策略仍在讨论。这里的 soft lockup 是 CPU 长时间无法正常调度的软锁死，不等同于已经证明存在锁依赖死锁。

## 问题背景

CoCo 架构下，CPU 能访问加密密钥而 IO 设备不能；设备的传统 DMA 只能写入未加密的 SWIOTLB bounce buffer（共享区），数据要进入加密的 guest 私有内存，必须由 CPU 在 bounce 区与私有内存之间拷贝（PDF 第 4 至 6 页）。注意这条 CPU 内存路径的加密由内存加密硬件处理，并非 SWIOTLB 做软件 AES，也不是新增 DMA 引擎。

对 NVMe 读 IO 而言，硬件 DMA 先把数据写入共享 bounce buffer，DMA 完成后才进入中断处理，随后在 DMA unmap 阶段由 SWIOTLB 把内容从 bounce 拷贝到私有加密内存（PDF 第 7 至 8 页）。`nvme_irq()` 先 poll 完成队列、汇集已完成请求形成 IO batch，再调用 `nvme_pci_complete_batch()`，其中经由 `nvme_pci_unmap_rq()` 触发 unmap 与 bounce 拷贝。传统实现把这些同步地放在硬中断上下文执行（PDF 第 7 至 8、17 页）。

每个 NVMe IO 队列有专用中断向量，向量钉到单个 CPU。轻负载时拷贝短，CPU 还有空闲喂 watchdog；高带宽读时单个 batch 很大，拷贝占满该 vCPU 的全部硬中断时间，后续中断被 APIC 锁存、没有 CPU 时间留给其他内核线程与 watchdog，出现 soft lockup（PDF 第 10 至 11 页）。讲者提到的既有缓解是把队列数从 4 增到 16、每队列深度从 512 降到 128，总槽位数仍为 2048，从而把拷贝分散到更多 vCPU，观测到的 soft lockup 消失；但这只是缓解：重拷贝仍在硬中断上下文，且受限于硬件存储队列数，存储提速后仍会再次饱和（PDF 第 13 至 15 页）。

## 核心技术与执行流程

提案只修改 `drivers/nvme/host/pci.c` 一个文件（RFC 为单 patch，+58/-3），不是对 SWIOTLB 的通用重写，也不引入异步 DMA 或新的异步 `dma_unmap` API。其流程为：`nvme_irq()` 仍 poll CQ、形成 batch；当模块参数 `force_bounce_swiotlb_wq` 开启、且该队列的 workqueue 存在、且 batch 非空时，把整个 completion batch 打包成 work item 排入队列专属的 `swiotlb_wq`；worker 线程随后执行 `nvme_pci_complete_batch()`，沿原有 unmap 与请求完成路径触发 SWIOTLB bounce 拷贝（PDF 第 17 页）。

几点需要准确说明。第一，拷贝仍由 guest CPU 完成，相关请求的硬件 DMA 已先完成，延迟的是包含 CPU 拷贝的完成处理，并非新增 DMA 引擎或省略拷贝。第二，必须在向上层宣布数据就绪、释放或复用相关缓冲区之前完成必要拷贝，这是正确性要求，不能据此声称补丁已存在缺陷。第三，workqueue 使用 `WQ_HIGHPRI | WQ_UNBOUND`，工作不再固定在 IRQ 绑定的 vCPU 上，但实际执行范围仍受 workqueue 配置和 CPU/NUMA 亲和性限制；`WQ_HIGHPRI` 使用高优先级 worker，不表示普通低优先级后台任务，也不保证 work item 间的完成顺序。关键变化是移到可调度的线程上下文。第四，参数 `force_bounce_swiotlb_wq` 默认关闭；该条件分支不自动检测每个请求是否实际使用 SWIOTLB，也没有只筛选读 batch，因此不能把原型称为通用的自动识别方案。

退化与边界方面：work item 用 `GFP_ATOMIC` 分配，分配失败则退回硬中断内同步完成，因此在极端内存压力下仍可能在硬中断做重拷贝。队列销毁时 `destroy_workqueue`、suspend 时 `flush_workqueue`。该机制解决的是中断钉住 vCPU、watchdog 被饿死导致的 soft lockup，并不能证明对所有 SWIOTLB 调用者、设备或 DMA 方向都适用，也不是对死锁的通用免疫。

## 性能结果与适用范围

PDF 第 18 页标注 Benchmark (40 runs)，表格以 p5 与 p50 两列列出相对变化；行项中的 clat p50/p90/p99.9 则指完成延迟的不同分位，不应与列标题混为同一层统计。材料没有充分解释列分位的具体统计对象和计算方法。PDF 中延迟正数标红、负数标绿，即增加延迟为回退、降低延迟为改善；RFC 邮件另用“正号为性能改善”的归一化口径，延迟项符号相对 PDF 取反，数值和改善/回退方向一致。本文保留 PDF 的指标变化方向，并用“增加/降低”明确表达延迟，避免仅凭正负号判断收益。

| Guest 配置 | IOPS p5 / p50 | 带宽 p5 / p50 | clat p50 p5 / p50 | clat p90 p5 / p50 | clat p99.9 p5 / p50 |
|---|---:|---:|---|---|---|
| 4 vCPU / 2 队列 | +7.59% / +4.05% | +1.53% / +1.18% | 增加 10.41% / 增加 3.88%（回退） | 降低 1.74% / 降低 5.51%（改善） | 降低 16.44% / 降低 9.86%（改善） |
| 176 vCPU / 4 队列 | +90.32% / +93.32% | +9.57% / +10.68% | 增加 9.58% / 增加 8.70%（回退） | 增加 9.91% / 增加 7.51%（回退） | 增加 8.00%（回退）/ 降低 4.58%（改善） |

解读要点：IOPS 与带宽在两组配置下均上升（改善），大 VM 增幅显著。延迟则好坏参半：小 VM 的 p50 延迟回退，但 p90 与 p99.9 改善；大 VM 的 p50、p90 及 p99.9 的 p5 分位回退，只有 p99.9 的 p50 分位改善。不能概括为"大 VM 所有延迟均改善"，也不能概括为"延迟全面回退"。讲稿把这些回退是否可接受、是否按 vCPU 数或用户参数条件启用，列为开放问题（PDF 第 18 页）。

适用范围与限制：基准只披露两种 VM/队列配置、40 次、p5/p50 与相对变化，未给出 CPU 型号、主机代际、存储设备型号、NVMe 型号、队列深度初值、fio 配置、块大小、读模式、数据集与缓存状态、运行时长、方差或置信区间，也没有原始绝对值；40 次运行本身的独立性、p5/p50 的具体统计对象与计算方法未充分定义。因此结果不可外推为通用性能保证。指标是工作负载性能而非 CPU 利用率或周期，基准未量化 CPU 资源节省；标题中的"降低高 CPU 利用率"指的是缓解硬中断上下文对被钉 vCPU 的独占式饱和，不是基准测得的 CPU 使用率下降。两组配置的 vCPU 数与队列数都不同，不能用以推断纯 vCPU 扩展效应。RFC 文字称在 40 次高带宽连续运行中，基线频繁出现 soft lockup、延迟实现未出现；该声明来自作者，且受 `GFP_ATOMIC` 失败回退限制，不构成对所有情形的保证。

## 进展与未决问题

RFC 于 2026-10-06 发出，报告安排在 2026-10-07；材料以原型和提案为基础，未提供已合入主线的证据。本文不是当前发行版代码审计。官方摘要讨论是否默认启用、是否面向强制 SWIOTLB 的 VM、是否按 vCPU 数启用，以及用户如何配置；幻灯片第 18 页则询问性能回退能否接受、是否条件启用和是否有更好方法。上述策略均未定论。需要验证的边界包括分配失败时退回 IRQ 同步拷贝、异常完成、队列销毁、并发时序及排队延迟；材料未展示这些场景的完整测试，不表示已经确认存在缺陷。

## 后续发展启示（分析）

以下为基于现有材料的分析性推断，标注为分析，非已确认结论。

1. 把长 CPU 工作移出硬中断，改善可调度性：必要的拷贝并未消失；排队、调度、缓存与 NUMA 行为还可能改变总 CPU 成本。因此应测量每单位 I/O 的 CPU 时间或周期、IRQ 占用和其他任务进度，不能直接断言整体 CPU 使用率下降。
2. 吞吐与中位/尾延迟的权衡需要策略：大 VM 在 IOPS 与带宽上获益巨大，但 p50 与 p90 延迟回退，最优策略取决于负载是吞吐敏感还是延迟敏感，这正是启用策略难以一刀切的原因。
3. NUMA 亲和、内存压力与拆除路径有待验证：非绑定 workqueue 可能把工作调度到其他 vCPU 或 NUMA 节点，带来 NUMA 亲和与缓存效应，但未测量；`GFP_ATOMIC` 失败回退、suspend flush 与 destroy 路径存在，完整并发与拆除验证尚未展示，这些是未来验证方向，非已确认缺陷。
4. 两组配置不能隔离规模因素：4 vCPU 配 2 队列、176 vCPU 配 4 队列，vCPU 数与队列数同时变化，无法据此推断纯 vCPU 扩展曲线，也无法分离队列数对结果的影响。
5. SWIOTLB API 仍是同步非阻塞：补丁只改 NVMe 完成调度，不改 SWIOTLB 的 map/unmap 接口本身；从调用者看 SWIOTLB 的 unmap 仍是非阻塞同步语义，这不是通用的异步 `dma_unmap` 或 SWIOTLB 重写。

## 参考资料

1. [LPC 2026 官方报告页面（contribution 2475）](https://lpc.events/event/20/contributions/2475/)：年份、微会议、讲者、时间与摘要入口。
2. [官方 PDF](https://lpc.events/event/20/contributions/2475/attachments/2272/4966/06-LPC2026-SWIOTLB-bounce-deferring.pdf)：第 4 至 8 页机制与中断阶段，第 10 至 11 页软锁死时序，第 13 至 15 页既有缓解，第 17 页提案，第 18 页基准与开放问题。
3. [RFC 补丁邮件（lore.kernel.org）](https://lore.kernel.org/all/20261006143329.2415252-1-afranji@google.com/)：单 patch，含 unmap 与读拷贝语义、workqueue 触发条件、`GFP_ATOMIC` 回退、40 次 soft lockup 声明与归一化口径的基准说明。
4. [SWIOTLB 内核文档](https://docs.kernel.org/core-api/swiotlb.html)：SWIOTLB bounce buffer 的通用背景。
