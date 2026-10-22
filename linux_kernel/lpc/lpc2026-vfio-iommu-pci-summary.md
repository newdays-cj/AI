# LPC 2026 VFIO/IOMMU/PCI 微会议纪要

## 范围与证据说明

- 研究日期：2026-10-14
- 会议：Linux Plumbers Conference 2026，2026 年 10 月 5 日至 7 日，布拉格
- 微会议：VFIO/IOMMU/PCI Microconference
- 时间与地点：2026 年 10 月 5 日，时区 Europe/Prague，会场 Club E
- 官方议程页：https://lpc.events/event/20/sessions/261/

证据来源与写作约定：

- 本文摘要基于各议题的官方幻灯片与摘要整理，并非逐字逐句的会议记录，也非对线上代码库的实时审计。
- 文中涉及的目标（target）、RFC 与已合入主线（merged）的内容严格区分，不混为一谈；除非明确标注已合入，否则均未声明合入状态。
- 涉及分析、推断或主观判断之处，文中会标注为分析，与客观陈述区分；分析不代表社区共识。
- 本年度报告不罗列往年成果清单，仅聚焦本届会议讨论的当前进展。
- 各议题的资料基础（官方幻灯片页数与版本）在每节标题下方单独标注。

## 核心结论

八个议题共同反映出 PCI/VFIO/IOMMU 正从设备枚举和 DMA 映射基础设施，发展为协调可信 I/O、外部翻译所有权、设备间数据路径与动态拓扑的系统接口。当前进展既有已合入的具体修复，也有可运行原型、评审中的补丁与尚未确定的接口策略，不能用一个统一的“已支持”标签概括。

本文的分析主线是：先明确谁控制资源和翻译，再建立可观测性，最后为热插拔、复位、失效和回收定义可验证的生命周期契约。性能与功能结果必须同时保留适用条件和测试覆盖边界。

## 议程

| 序号 | 时间 | 议题 | 演讲者 | 贡献链接 |
| --- | --- | --- | --- | --- |
| 1 | 15:00 | [Secure vIOMMU architecture and implementation challenges](#talk-1) | Suravee Suthikulpanit / Vasant Hegde | https://lpc.events/event/20/contributions/2522/ |
| 2 | 15:25 | [IOMMU Page Table Observability and Reclamation](#talk-2) | Logan Odell / Pranjal Shrivastava | https://lpc.events/event/20/contributions/2624/ |
| 3 | 15:45 | [IOMMUFD Support for Microsoft Hypervisor Hosts](#talk-3) | Jacob Pan | https://lpc.events/event/20/contributions/2520/ |
| 4 | 16:10 | [arm-smmu contig hint support and performance impact](#talk-4) | Prakash Gupta / Vijayanand Jitta | https://lpc.events/event/20/contributions/2625/ |
| 5 | 17:00 | [VFIO PCIe TPH Userspace API & Progressive Security Policy](#talk-5) | Chengwen Feng | https://lpc.events/event/20/contributions/2623/ |
| 6 | 17:20 | [Two Years of Making PCIe Hotplug Reliable: What Landed, What Didn't, and Where Do We Go](#talk-6) | Maciej Grochowski | https://lpc.events/event/20/contributions/2519/ |
| 7 | 17:45 | [Describing P2PDMA capabilities in ACPI](#talk-7) | Leon Romanovsky / Lukas Wunner | https://lpc.events/event/20/contributions/2508/ |
| 8 | 18:10 | [Testing the Untestable: Using QEMU for Testing PCI Endpoint Framework](#talk-8) | Manivannan Sadhasivam | https://lpc.events/event/20/contributions/2384/ |

<a id="talk-1"></a>
## 1. Secure vIOMMU architecture and implementation challenges

- 英文标题：Secure vIOMMU architecture and implementation challenges
- 时间：2026-10-05 15:00 Europe/Prague
- 演讲者：Suravee Suthikulpanit / Vasant Hegde
- 资料基础：官方幻灯片 17 页，分支标记 v1，非会议逐字记录
- 贡献链接：https://lpc.events/event/20/contributions/2522/

### 问题背景

AMD SEV-SNP 机密虚拟机需要将可信 I/O（TIO）设备直接分配给虚拟机，并允许设备直接访问其私有内存，实现安全 DMA。关键约束在于不可信的 Hypervisor 不能控制安全配置路径：传统由 Hypervisor 编程设备表的方式无法直接满足这一信任边界。议题核心是如何在保留 Hypervisor 资源分配职责的同时，把安全设备表（secure DTE）的控制权交给 AMD 安全处理器（ASP），并阻断 Hypervisor 对安全 guest 的 VF Control 读写（PDF p.2, p.5）。

### 核心技术

可信设备使用独立的安全设备表，每个安全 DTE 为 512 比特条目，包含客户页表信息与相关状态，由 ASP 独占拥有并以加密内存编程，Hypervisor 无法读写（PDF p.3）。安全客户通过 GHCB 向 ASP 请求安全 DTE 与命令/PPR 缓冲区配置；ASP 校验请求、编程 IOMMU 后备内存与域/设备 ID 表，并管理 RMP 状态。不可信的 Hypervisor/VMM 仅负责分配后备页与域 ID/设备 ID 表内存、为每个 TDI 分配域 ID、将 VF MMIO 页分配给客户（PDF p.6）。

RMP 的所有权与执行是分离的：ASP 管理 RMP 状态，IOMMU 硬件遍历嵌套页表并依据 RMP 表校验访问（PDF p.6, p.14）。IOMMU 宿主页表须与 CPU 页表同步，且宿主页表条目粒度须匹配对应 RMP 条目粒度，同步须在安全客户 DMA 启用前完成。当前原型使用通用页表的 cut_mapping() 拆分页表条目，基于 Jason 的原型代码，是否有更优机制仍开放（PDF p.10）。

驱动结构方面，当前设计使用 ops 结构（guest ops 与 sev_guest 通信、host ops 与 crypto/CCP 通信），以 EXPORT 函数作为备选方案仍在讨论中（PDF p.8）。接口方面，VIOMMU_INIT 路径需要 kvmfd/struct kvm 引用以获取 SNP 客户上下文、ASID 等 kvm_sev_info 数据；当前原型通过 IOMMU_VIOMMU_ALLOC 传递 kvmfd，计划迁移到 viommu_init 接口（其 iommufd_device 携带 kvm_file 引用），基于 Aneesh 的 RFC v6（PDF p.9）。

### 进展与未决问题

内核与 QEMU 各有可工作的原型，幻灯片列出的分支均为 `sviommu/tsm0407_v619_v1`，建立在 AMD hw-vIOMMU 系列之上，后者仍在上游评审中（PDF p.7）。安全 vIOMMU 补丁计划在 hw-vIOMMU 特性被接受之后再行投递，不能据原型可运行推断已合入。ops 与 EXPORT 的取舍、kvmfd 到 viommu_init 的迁移仍属讨论阶段；材料未给出性能基准（PDF p.7, p.8, p.9）。

### 发展启示（分析）

安全 vIOMMU 的设计本质是在信任边界上做减法：Hypervisor 保留资源分配职责，安全路径控制权完全交给 ASP。这种分割带来一个结构性约束，即宿主页表粒度必须与 RMP 粒度对齐，cut_mapping() 当前只是原型级答案，更优的页表拆分机制仍开放。从上游节奏看，安全补丁完全依赖 hw-vIOMMU 系列的接受进度，合入时间线不可独立预测。ops 与 EXPORT 的取舍虽是实现细节，却折射出 IOMMU 驱动在 guest/host 通信路径上的长期架构选择。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2522/
- 议程 PDF：https://lpc.events/event/20/contributions/2522/attachments/2080/4643/LPC2026_svIOMMU_upload_v1.pdf
- 页码引用：p.2, p.3, p.5, p.6, p.7, p.8, p.9, p.10, p.14

<a id="talk-2"></a>
## 2. IOMMU Page Table Observability and Reclamation

- 英文标题：IOMMU Page Table Observability and Reclamation
- 时间：2026-10-05 15:25 Europe/Prague
- 演讲者：Logan Odell / Pranjal Shrivastava
- 资料基础：官方幻灯片 15 页
- 贡献链接：https://lpc.events/event/20/contributions/2624/

### 问题背景

VFIO/IOMMUFD 的用户态映射允许 VMM、guest 或设备驱动选择 IOVA，稀疏布局和反复 map/unmap 容易留下大量中间页表。报告描述的现有路径通常只清除叶子 PTE；部分 unmap 将目录清空后，目录及父级仍可能驻留，缺少空检查和递归回收。材料指出这种滞留可达 GB 级，但没有给出对应基准配置；同时只有全局页表记账，难以追踪资源归属（PDF p.2, p.5, p.7）。

### 核心技术

可观测性 RFC 提议在通用页表分配路径使用 `iommu_alloc_pages_attributed()`，在分配/释放时按 domain 计数。`struct ioptdesc` 关联 `struct iommu_domain`，每个域增加 `atomic_long_t nr_pages` 记录页表页数。读取 `/proc/<pid>/fdinfo/<FD>` 时聚合该描述符背后所有域的计数，输出例如 `iommu-nr-pages: 11`；这是接口示例，不是性能测量（PDF p.9）。

回收方面，通过核心 MM shrinker API 异步触发延迟回收。仅 L4/叶子目录页被引用计数，复用原生 page->_refcount 经 ioptdesc 别名。当叶子目录 refcount 降至恰好 1 时，它为空并被记录到每域 XArray 中。shrinker 从 XArray 移除候选者，用 atomic_cmpxchg(&refcount, 1, 0) 认领，在 L3 父槽上 cmpxchg 以 0x0 替换从而切断分支，随后调用 synchronize_srcu() 等待瞬态软件遍历者，最后恢复 refcount 为 1 并将页释放回 buddy（PDF p.12, p.13）。

状态机三态：Active 为 refcount > 1（存在有效叶子 PTE）；Stranded candidate 为 refcount == 1（所有叶子条目无效，L4 页记入 XArray）；Claimed 为 refcount == 0（shrinker 赢得 cmpxchg(1, 0) 并切断父指针）。若 mapper 先执行 atomic_inc_not_zero() 使 refcount 升至 2，shrinker 认领失败；若 shrinker 先认领，mapper 自增失败，退出 SRCU 读区在父移除后重试并分配新 L4 页（PDF p.13）。仅引用计数 L4/叶子目录可避免共享上层 L1/L2 描述符上的缓存行争用；在完全填充的树中超过 99.8% 的页是 L4 目录（PDF p.14）。

### 进展与未决问题

可观测性和回收两项工作均为 RFC，材料未声明已经合入。IOTLB 与页遍历缓存失效仍是开放问题：在释放前调用 flush_iotlb_all() 还是使用基于范围的页结构失效尚未定论。SRCU 保护并发软件读取/遍历者，但本身不失效硬件翻译或页遍历缓存状态（PDF p.15）。DMA 域 map/unmap 可能在 IRQ 上下文执行，支持范围仍待确定；直接回收上下文中是否使用 call_srcu() 或 work item 延迟释放，以及 cgroup/进程归属记账也仍开放。材料未给出实测回收内存量、性能或延迟数据。

### 发展启示（分析）

该议题的核心张力在于回收路径须在不引入域级热路径锁的前提下，协调并发 map/unmap 与硬件页表遍历。XArray、cmpxchg 和 SRCU 分别承担候选记录、竞争仲裁和软件读者同步，但仅凭幻灯片不能证明所有并发路径都正确。后续验证必须同时覆盖软件生命周期和硬件缓存失效。在完成这些验证并给出回收与热路径成本数据之前，收益应作定性描述。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2624/
- 议程 PDF：https://lpc.events/event/20/contributions/2624/attachments/2085/4737/LPC%27_26_%20IOPT%20Observability%20%26%20Reclamation.pdf
- RFC：https://lore.kernel.org/all/20261001224531.765278-1-praan@google.com/ ; https://lore.kernel.org/all/20261001230219.818128-1-praan@google.com/
- 页码引用：p.2, p.5, p.7, p.9, p.12, p.13, p.14, p.15

<a id="talk-3"></a>
## 3. IOMMUFD Support for Microsoft Hypervisor Hosts

- 英文标题：IOMMUFD Support for Microsoft Hypervisor Hosts
- 时间：2026-10-05 15:45 Europe/Prague
- 演讲者：Jacob Pan
- 资料基础：官方幻灯片 16 页
- 贡献链接：https://lpc.events/event/20/contributions/2520/

### 问题背景

在 Microsoft Hypervisor（MSHV）架构中，MSHV 拥有物理硬件 IOMMU，Linux 根分区拥有 PCI 设备，客户虚拟 IOMMU 仿真在 MSHV 中而非用户态 VMM。IOMMU 的 stage-2 I/O 页表（S2 IOPT）可与 CPU SLAT 共享。Linux 通过宿主 PV-IOMMU 驱动使用 map/unmap 超调用编程硬件 IOMMU 的 S2，定性描述为昂贵（PDF p.5）。现有模型中设备附加到 IOMMUFD 内创建的 nesting-parent S2 域，新模型需要设备附加到由 Hypervisor 管理、位于宿主 IOMMUFD 外部的 S2（PDF p.6）。

### 核心技术

外部附加（external attach）模型提出新的 domain 类型、vIOMMU 类型和 HWPT 对象，并复用 vDEVICE（PDF p.9, p.10, p.11）。`IOMMU_DOMAIN_EXTERNAL` 是由外部 owner 管理的非分页域；这里表示 Linux 不直接管理其分页操作，而非 DMA 绕过硬件翻译，S2 仍由 MSHV 管理。`IOMMUFD_VIOMMU_TYPE_HYPERVISOR` 是每 VM 对象，持有 VM/分区文件引用以锚定生命周期，类型特定助手解析分区 ID 并协调 guest 内存 pin/映射。`IOMMUFD_OBJ_HWPT_EXTERNAL` 表示外部域的 IOMMUFD 对象。复用的 `IOMMUFD_VDEVICE` 将物理设备与其 guest 可见的逻辑设备 ID 关联。

外部附加解耦了嵌套附加：宿主通过超调用将物理设备附加到 S2，客户独立配置 S1 并与 MSHV 直接建立嵌套（PDF p.7, p.11）。用户态流程为：创建 MSHV 分区获取 vm_fd，用 vm_fd 分配 IOMMUFD_VIOMMU_TYPE_HYPERVISOR，分配 vdevice（其 vm_device_id/逻辑 ID 由 VMM 指定），分配外部 HWPT 并通过 VFIO 附加，宿主 PV-IOMMU 驱动随后调用 external-domain attach_device 操作，解析分区 ID 与逻辑设备 ID，可选地通过分区回调强制内存 pin，最终调用 MSHV attach-device 超调用（PDF p.11）。补丁状态：客户 PV-IOMMU 驱动 v5，宿主 PV-IOMMU 驱动 v2，IOMMUFD MSHV 扩展 RFC，PASID+SVA 为 WIP（PDF p.3）。

### 进展与未决问题

PASID/ATS 启用契约跨宿主附加与客户附加仍为 WIP，外部附加时宿主尚不知道 PASID 和 ATS 是否及如何被使用（PDF p.12）。Xen 是否可复用 external-attach 流程并共享 vIOMMU 助手与 TSM 仍开放。Live update 是否应将 /dev/mshv 注册到 LUO 会话以保留映射未定。P2PDMA 的可撤销 dma-buf 支持中 /dev/mshv 是否应作为 importer 仍开放（PDF p.13）。全文无任何定量性能数据，唯一的性能描述是 map/unmap 超调用昂贵（PDF p.5）。

### 发展启示（分析）

MSHV 的 external attach 模型把 IOMMUFD 的抽象从自管 S2 转向外部管 S2，通过引入非分页域句柄来表达外部翻译所有权。这保留了 IOMMUFD 的对象模型完整性，但也意味着 S2 的实际翻译逻辑在 IOMMUFD 视野之外。分区与附加设备生命周期对齐、客户内存 pin/映射协调是跨组件的硬约束，目前通过 vIOMMU 对象的回调机制表达，尚无性能验证。ATS/PASID 契约的缺失说明该模型尚未覆盖完整的安全分配语义。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2520/
- 议程 PDF：https://lpc.events/event/20/contributions/2520/attachments/2118/4692/LPC2026-IOMMUFD-MSHV.pdf
- 页码引用：p.3, p.5, p.6, p.7, p.9, p.10, p.11, p.12, p.13, p.15, p.16

<a id="talk-4"></a>
## 4. arm-smmu contig hint support and performance impact

- 英文标题：arm-smmu contig hint support and performance impact
- 时间：2026-10-05 16:10 Europe/Prague
- 演讲者：Prakash Gupta / Vijayanand Jitta
- 资料基础：官方幻灯片 14 页
- 贡献链接：https://lpc.events/event/20/contributions/2625/

### 问题背景

ARM SMMU 的 CONT 位允许硬件在 TLB 中合并多个 4 KiB 条目的覆盖范围，减少翻译查找压力。但 CONT 是一个提示而非保证，其使用受严格的对齐与属性约束，且部分取消映射的语义需要仔细处理以避免破坏 CONT 组完整性。议题探讨如何在 Linux IOMMU 页表层为 SMMU 启用 CONT 提示，以及它对实际工作负载的翻译开销影响。

### 核心技术

对于 4 KiB 粒度，一个 CONT 合格组由 16 个 4 KiB PTE 组成（16 x 4 KiB = 64 KiB），所有 16 个描述符标记 CONT=1 并保留在页表中。硬件可能缓存一个 64 KiB 覆盖条目，但 CONT 不会将它们替换为真正的巨型 PTE，也不保证只占一个 TLB 条目（PDF p.4, p.5, p.14）。CONT 要求整组合格：组大小对齐的 IOVA、连续 IOVA 范围、连续输出物理页、匹配的描述符属性、每个组成员都存在。仅物理连续不够（PDF p.6）。

arm_lpae_restrict_pgsizes() 将 CONT 组大小加入 pgsize_bitmap，提供退出选项 IO_PGTABLE_QUIRK_ARM_NO_CONT_HINT（PDF p.14）。取消映射时在组边缘检查实际 CONT 状态：部分组 unmap 在任何 PTE 被清除之前被拒绝，拒绝请求是完全 no-op；完整组 unmap 使用 TLB gather 加 BBM 清除/失效（PDF p.14）。

SMMU PMU 通过 perf 提供查找、命中和页表遍历计数器，使性能变化可归因到翻译行为而非依赖模糊的顶层开关 KPI（PDF p.8）。

### 进展与未决问题

最新补丁系列为 v5，2026-09-21 投递，在 linux-iommu 评审中（PDF p.10, p.12）。SMMUv3 的开放问题包括失效 errata MMU-700 #3777127 / SMMUv3 #3673557，以及使用 ARM_LPAE_PTE_DBM 的硬件脏跟踪（PDF p.12）。

对于测量的 AI 推理工作负载，CONT 使吞吐量提升超过 60%，CONT-on 运行显示 64 KiB 映射和 92.8% 叶子覆盖率，MTLB/TLB 和遍历缓存查找更少，页表遍历更少（PDF p.9, p.11）。这些数字仅适用于此特定工作负载；幻灯片未提供硬件/平台描述、绝对或显式基线值、运行次数或方差/误差数据。

### 发展启示（分析）

CONT 提示的价值在于以零页表压缩代价换取 TLB 合并的可能，但这种可能受硬件实现自由度约束，软件不能假设合并一定发生。92.8% 叶子覆盖率和 60% 吞吐提升是单一工作负载的观测值，外推到其他场景缺乏依据。SMMU PMU 的引入是正确的方法论选择：把 on/off 实验归因到翻译行为层面，而非仅看顶层吞吐数字。部分组 unmap 的前置拒绝策略保证了 CONT 组完整性，但代价是被拒绝的 unmap 成为 no-op，调用者须自行处理。v5 仍在评审且 SMMUv3 errata 未解，距离合入仍有距离。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2625/
- 议程 PDF：https://lpc.events/event/20/contributions/2625/attachments/2010/4543/LPC2026_SMMU_CONT_hint.pdf
- 页码引用：p.4, p.5, p.6, p.8, p.9, p.10, p.11, p.12, p.14

<a id="talk-5"></a>
## 5. VFIO PCIe TPH Userspace API & Progressive Security Policy

- 英文标题：VFIO PCIe TPH Userspace API & Progressive Security Policy
- 时间：2026-10-05 17:00 Europe/Prague
- 演讲者：Chengwen Feng
- 资料基础：官方幻灯片 14 页
- 贡献链接：https://lpc.events/event/20/contributions/2623/

### 问题背景

PCIe TLP Processing Hints（TPH）允许设备用 Steering Tag（ST）将 DMA 流量导向特定缓存，降低延迟。在虚拟化直通场景下，把 ST 编程能力暴露给不可信客户带来安全考量：主要风险是跨租户缓存污染或性能侧信道干扰，而非内存访问或 DMA 权限绕过。议题设计一套渐进式安全策略，在 VFIO 用户态 API 中分层次暴露 TPH 能力。

### 核心技术

三个设备特性通过单个 VFIO_DEVICE_FEATURE ioctl 的不同特性选择器暴露，而非三个独立 ioctl（PDF p.6, p.7, p.12）。VFIO_DEVICE_FEATURE_TPH（特性 ID 14）用于 SET 每设备 opt-in 与 GET 能力；VFIO_DEVICE_FEATURE_TPH_RESOLVE（ID 15）仅 GET，解析 PH/ST；VFIO_DEVICE_FEATURE_TPH_ST（ID 16）仅 SET，批量编程架构化 ST 条目（PDF p.12, p.13, p.14）。

安全模型为双层：全局 tph_policy 闸门加每设备 opt-in。tph_policy=0（默认，No-ST）：DMABUF 解析只返回 PH，ST 编程不可用。tph_policy=1：受控 ST 编程（NONE/DMABUF/CPU 源），RESOLVE 仍只返回 DMABUF PH，MSI-X 表 ST 风险存在。tph_policy=2：额外暴露原始 ST 解析（DMABUF/CPU 源），风险扩展到 MSI-X、配置空间和设备特定 ST（PDF p.7）。

安全前提假设设备非恶意而整个客户 VM 不可信。风险随 ST 机制变化：未捕获的 MSI-X 表写入无法校验，配置空间 ST 可被捕获并校验，设备特定 ST 缺少通用校验路径（PDF p.5）。用户态须先 SET VFIO_DEVICE_FEATURE_TPH 为该设备授权，然后才能使用能力 GET、RESOLVE/ST 或访问虚拟化 TPH 能力寄存器。ST 编程前还须写 TPH 控制寄存器激活设备硬件 TPH 逻辑（PDF p.12, p.14）。ST 条目在 PCI reset 后不保留，MSI-X 空间 ST 条目被清除且不被 PCI 核心状态保存/恢复，用户态须在每次 reset 后重新编程所有所需 ST 条目（PDF p.14）。

### 进展与未决问题

ST 目标生命周期是上游阻塞点：DMABUF 有效到无效转换与 ST 变更可留下陈旧 ST 值，导致缓存污染或性能干扰。检测目标变更、定位受影响设备/索引条目、失效或更新硬件 ST 条目仍开放。两种可能（用户态跟踪或内核跟踪）和反向映射与设备特定通知均未解决（PDF p.8）。作者推荐用户态跟踪以获得一致语义并避免内核跨子系统/引用计数复杂性，但这作为推荐提出，生命周期跟踪仍开放，非最终社区共识（PDF p.9）。

开发从 4 月 RFC 推进到 7 月 v20，目前仍在社区评审/上游迭代中。v21 计划在为 DMABUF 解析 ST 前调用 pci_p2pdma_distance()。未给出确认的内核合入版本。未来项包括硬件无关自动化 selftest、TPH 虚拟化和 DPDK/SPDK 风格用户态 SDK 集成。无具体 QEMU 实现计划，无基准测试结果（PDF p.6, p.10）。

### 发展启示（分析）

渐进式安全策略的核心洞察是把 ST 暴露与风险等级绑定：默认 No-ST、受控 ST、原始 ST 三级对应逐步扩大的攻击面。这种设计承认缓存干扰与 DMA 权限绕过是不同类别的威胁，用策略闸门而非能力开关来表达。三个特性共用一个 ioctl 框架是对早期独立 ioctl 设计的收敛，减少了接口碎片。ST 目标生命周期问题暴露了 TPH 元数据缺少 invalidate_mappings 生命周期钩子的结构性缺口，作者推荐的用户态跟踪方案虽合理但非共识，说明该问题的解法仍有争议空间。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2623/
- 议程 PDF：https://lpc.events/event/20/contributions/2623/attachments/1989/4507/VFIO%20PCIe%20TPH%20Userspace%20API%20-%20Progressive%20Security%20Policy.pdf
- 页码引用：p.5, p.6, p.7, p.8, p.9, p.10, p.12, p.13, p.14

<a id="talk-6"></a>
## 6. Two Years of Making PCIe Hotplug Reliable: What Landed, What Didn't, and Where Do We Go

- 英文标题：Two Years of Making PCIe Hotplug Reliable: What Landed, What Didn't, and Where Do We Go
- 时间：2026-10-05 17:20 Europe/Prague
- 演讲者：Maciej Grochowski
- 资料基础：官方幻灯片 15 页，报告版本而非独立审计
- 贡献链接：https://lpc.events/event/20/contributions/2519/

### 问题背景

PCIe 已从单机 I/O 演变为机架级生产互连。Sony 当前一代机架内含 100 多个 PCIe 端点、2 到 3 层交换，支持热插拔和点对点通信；下一代面向 Gen6、1000 多个端点、更大的 MMIO 窗口，以及 NVMe、计算模块、DPU、NTB 混合拓扑（PDF p.2, p.3）。团队原先在 6.1 私有分支携带树外修复，升级后正常枚举看似成功，但节点断电触发 DPC 恢复时出现 UAF，反映补丁回移与对象生命周期保护的缺口（PDF p.4）。本报告中的 Hotplug 指运行期间的拓扑变化，不限于物理拔插（PDF p.1）。

### 核心技术

第一条主线是 DPC 与 AER 解耦：旧实现把 DPC 闸门挂在每端口的 AER-service 位上，交换下行口因此静默丢失原生 DPC；v7.3 提交 97ca178c899d 改为按宿主对原生 AER 的所有权判定，修复交换下行口的 DPC（PDF p.5, p.14）。第二条是引用化 reset-set 快照：旧流程 walk 加 lock 后 RESET 再 walk 加 unlock，两次遍历之间设备可能被移除，锁集与解锁集不一致；RFC 改为一次快照加引用，同一组对象、同一顺序贯穿复位（PDF p.6, p.10）。第三条是 MMIO 求和与对齐偏差：Sony 八端点各请 20 MiB，原始合计 160 MiB，但 16 MiB 对齐让每项实占可达 32 MiB，现场观测跨度 244 MiB 只装下 8 个中的 5 个，按对齐折算需 256 MiB，三个数值并不相等（PDF p.7, p.15）。第四条是嵌套延迟子树：T0 硬复位清空父子交换，T1 父先就位只分到约 3 条总线约 206 MiB，T2 子树迟到却要 11 条总线、1032 MiB 且 1 GiB 对齐，资源在子树出现前已提交，Switchtec 无法绑定（PDF p.9, p.15）。第五条是 MPS：根端口与交换路径 512，新端点 MPSS 128，路径契约不匹配带来 malformed-TLP 风险，热加到在用交换之下无法安全降 MPS，更需层级范围内的活 DMA 静默（PDF p.6, p.8）。

### 进展与未决问题

已合并的是点状修复（PDF p.5）：v6.11 11a1f4bc4736 修复 pci_dev_wait 子设备 UAF；v6.16 2af781a9edc4 等抑制 reset/DPC 链路事件；v6.18 45bc82563d55 等重做恢复状态机；v7.0 3958bf16e2fe 收窄桥窗过高估计；v7.1 a1ed752bc7cb 让 DPC 持端口引用、8238cb69c01f 使 reset_subordinate 热插拔安全、9036bd0efcb6 改善头空间对齐；v7.3 的 DPC/AER 解耦。这些落在具体漏洞上，并未改写子系统契约。

四条未完成工作按 p.6 状态区分：MMIO 放置 PATCH v2 处于 TESTING，跨机型验证中，尚未判定能否在不制造新回归的前提下修好 Sony 八端点；MPS 处于 RFC、BUILD-TESTED，扫描期编程已重构，但热加到在用交换下的策略未定；嵌套延迟子树 RFC 已 HW-TESTED，但只在两级 Switchtec 拓扑上验证，三层及以上如何描述未来需求仍开放；locking/lifetime RFC 属 DESIGN OPEN，快照修复了复现的总线复位竞态，拓扑所有权契约仍未定（PDF p.6, p.10）。Keith Busch 2024 的八补丁总线锁分支里，v6.13 合入的五条属于准备工作、未改锁语义，关键生命周期与锁定保证尚未完整合入（PDF p.13）。`pci=hpreserve=` 在两级拓扑上的有效性属于 RFC 验证结果，不能当作普遍可用的主线参数。

### 发展启示（分析）

已合入的具体修复显著减少了崩溃与误热插拔，但不能据此推断动态拓扑已经全面可靠。后续应明确复位和移除的互斥/协调规则、未来子树的资源需求描述，以及活动层级的安全重配置策略。测试须从冷启动枚举扩展到带负载复位、延迟子树和并发移除，并利用第二种拓扑验证通用性。Sony 提供硬件、复现负载、trace、KASAN/lockdep 和机群验证，报告希望落实一个优先级、一个下一步行动、一个跟进负责人；这说明验证协作与补丁本身同样重要（PDF p.11）。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2519/
- 议程 PDF：https://lpc.events/event/20/contributions/2519/attachments/2279/4997/lpc2026-two-years-of-making-pcie-hp-reliable.pdf
- 页码引用：p.2, p.3, p.5, p.6, p.7, p.8, p.9, p.10, p.13, p.14, p.15

<a id="talk-7"></a>
## 7. Describing P2PDMA capabilities in ACPI

- 英文标题：Describing P2PDMA capabilities in ACPI
- 时间：2026-10-05 17:45 Europe/Prague
- 演讲者：Leon Romanovsky / Lukas Wunner（合著者 Mahesh Natu）
- 资料基础：官方幻灯片 17 页，外加 4 页 HMAT ECN 标注草案
- 贡献链接：https://lpc.events/event/20/contributions/2508/

### 问题背景

Linux 对跨 Host Bridge 等 P2PDMA 路径的判断依赖平台白名单，持续增加平台条目带来维护负担（官方摘要）。路由能力还取决于拓扑、ACS 策略、ATS 翻译和地址类型等条件，而 guest 无法从虚拟 PCI 拓扑推断真实路径约束。议题探讨以 ACPI HMAT 扩展描述可达性、适用性和性能，并由 QEMU 将 hypervisor 掌握的物理关系转换为 guest 可消费的信息（PDF p.6, p.8）。目标是减少对白名单的依赖，不意味着报告时已经完成替换。

### 核心技术

方向与请求/完成路由是基础：内存读有请求（发起方 A 到 B）和完成加数据（B 到 A），Posted 写有请求加数据（A 到 B）无完成 TLP。A 到 B 操作包含其返回流量，B 到 A 是独立操作（PDF p.3）。ACS 对请求和完成分别评估：请求在请求方分支上用 Redirect/Egress Control 评估，完成在目标方分支上用 Completion Redirect 和完成排序评估（PDF p.4）。

ATS 改变请求地址类型，但物理直达路由不足以保证可用映射。QEMU 须暴露 Hypervisor 知识，因为虚拟 PCI 拓扑隐藏了物理路径约束（PDF p.6）。HMAT 提供发起方到目标方矩阵的三层契约：可达性（发起方 A 能否对目标 B 完成操作）、适用性（哪些 TLP 和地址条件适用）、性能（哪个可用 provider 最适合传输）（PDF p.8）。

QEMU 将宿主桥映射为接近性域，SRAT 将每个桥关联到域 ID，HMAT 提供以发起方为行、目标方为列的二维矩阵，分别给出延迟和带宽。示例方向条目 A 到 B 为 900 ns、12 GB/s，B 到 A 为 600 ns、20 GB/s，这些明确标注为单事务类的示意值而非基准结果（PDF p.9）。提议的流程为：获取已验证的对端知识，将物理宿主设备标识与客户标识关联（宿主 A/B 映射为客户 A'/B'，宿主域映射为客户域），在客户域中保留方向与 TLP 条件区分，生成客户 SRAT 加扩展 HMAT（PDF p.11）。

关于规格状态：HMAT 扩展的 Type 3 结构（PCIe P2P 延迟与带宽信息结构）来自一份标注为 ACPI ECN 草案的文档，并非已批准的规范文本（ECN PDF p.1）。待约定的语义包括正向条目保证、请求/完成条件、unknown 与显式 unsupported 的区分、缺失或矛盾记录的处理，以及域标识/生命周期规则。UIO、请求地址类型和完成 RO 是三个不同的适用性维度（PDF p.16, p.17）。

### 进展与未决问题

提议的下一步是 QEMU 生成 HMAT 加客户事务匹配，但开放问题涵盖 Hypervisor 对端知识来源、所需 HMAT 属性/保证，以及不完整信息处理（PDF p.12）。Type 3 ECN 草案的语义尚未在规范层面达成一致，包括正向条目保证、请求/完成条件、unknown 与 explicit rejection 的区分、缺失或矛盾记录，以及域标识/生命周期规则（PDF p.16）。验证要求包括交换发起方与目标方时保留不同答案、改变地址类型或完成 RO 后匹配记录、区分无描述/unknown 与显式 unsupported、分配客户域后保留关系，以及测试 ATS 全局/每映射缓存路由答案（PDF p.17）。

### 发展启示（分析）

该议题的深层贡献是把 P2PDMA 从二值可达性描述升级为方向敏感的三层矩阵。早期 RFC 的单一非 UIO 回退结果不足以表达真实路由条件，每条事务结果须保持独立。QEMU 物理标识到客户 SRAT/扩展 HMAT 的映射思路合理，但其正确性完全依赖 Hypervisor 对端知识的准确性与完整性。Type 3 ECN 草案未获规范批准意味着实现层面缺少合同保证，unknown 与 unsupported 的语义区分在工程上不可省略。示意值（900/600 ns、12/20 GB/s）不构成基准，仅说明方向不对称的表达需求。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2508/
- 议程 PDF：https://lpc.events/event/20/contributions/2508/attachments/2042/4584/hmat-p2pdma-virtualization.pdf
- ECN 标注草案：https://lpc.events/event/20/contributions/2508/attachments/2042/4585/HMAT%20Extensions%20for%20PCIe%20P2P%20-%20external.pdf
- 页码引用：p.3, p.4, p.6, p.8, p.9, p.11, p.12, p.16, p.17

<a id="talk-8"></a>
## 8. Testing the Untestable: Using QEMU for Testing PCI Endpoint Framework

- 英文标题：Testing the Untestable: Using QEMU for Testing PCI Endpoint Framework
- 时间：2026-10-05 18:10 Europe/Prague
- 演讲者：Manivannan Sadhasivam
- 资料基础：官方幻灯片 27 页
- 贡献链接：https://lpc.events/event/20/contributions/2384/

### 问题背景

Linux PCI Endpoint Framework 让运行 Linux 的系统作为 PCIe Endpoint，EPC 层控制端点控制器，EPF 层实现设备功能，ConfigFS 将两层绑定。它是端点功能基础设施，而非仅用于测试的框架。现有 kselftest 通常需要通过 PCIe 连接两台 Linux 系统并控制两端，难以接入常规 CI。本报告探索用 QEMU 降低软件功能验证对实体硬件的依赖（PDF p.3, p.4）。

### 核心技术

初始设计使用两个 QEMU 实例通过 Unix socket 连接：一个 Linux 客户作为 Endpoint，另一个作为 Root Complex/宿主。Endpoint MMIO 操作通过 pcie-ep-ctrl 和 socket 转发到 pcie-ep-proxy（PDF p.7, p.8, p.9）。Kselftest 结果为 10 pass、10 fail、3 skip，过滤后为 10 pass、0 fail。这不是全部测试通过的结果。DMA、INTx 和 Doorbell 功能缺失（PDF p.8）。

QEMU 维护者建议重新考虑双实例设计，因为 socket 和两个地址空间增加了复杂度。替代方案使用一个 QEMU 实例、一个内核、一个客户地址空间，通过进程内调用和 BAR 内存别名实现。更简单且代码量更少。QEMU v1 系列已投递（PDF p.11, p.12, p.13）。单实例设计的测试结果相同：10 pass、10 fail、3 skip，过滤后 10 pass、0 fail，同样不是全部通过。DMA、INTx 和 Doorbell 仍不支持（PDF p.14, p.15）。

单实例模型中 Endpoint Controller 和 Root Complex 共存于一个 QEMU 实例，整合 pci-epf-test、pci-ep-generic EPC 驱动、pcie-ep-ctrl、pci_endpoint_test 宿主驱动、root port 和通用 endpoint function（PDF p.15）。

### 进展与未决问题

单实例方案无法独立测试以下场景：Endpoint 重启或崩溃而宿主保持运行、链路断开与链路重训练事件、热复位、意外移除、地址空间隔离（BAR 别名可能隐藏错误翻译），以及不同宿主与 Endpoint 内核（PDF p.17, p.18, p.19, p.20, p.21）。

TODO 项包括：添加 QEMU 功能测试并在 CI 中自动化 kselftest 执行、将覆盖范围扩展到 pci-epf-test 之外（包括 MHI 和 NVMe）、上游通用 EPC Linux 内核驱动、填补 DMA/INTx 路由/Doorbell 功能缺口（PDF p.23, p.24, p.25, p.26）。QEMU endpoint 仿真工作和通用 EPC/CI 测试支持工作仍不完整或未全部合入。

### 发展启示（分析）

QEMU PCI Endpoint 仿真降低了测试硬件门槛，单实例也有利于简化维护和自动化。但 BAR 别名可能隐藏错误翻译，不能据软件测试通过推断独立地址空间和真实链路行为正确。过滤不支持项后的 10 pass、0 fail 应与原始 10 pass、10 fail、3 skip 及缺失功能一起报告。这是一套有明确覆盖边界的测试方案：适合补充软件回归测试，独立端点崩溃、链路重训练、热复位、意外移除和不同两端内核仍需要其他模型或实机验证。

### 资料来源

- 官方议程页：https://lpc.events/event/20/contributions/2384/
- 议程 PDF：https://lpc.events/event/20/contributions/2384/attachments/2148/4751/Testing%20the%20Untestable%20-%20QEMU%20PCI%20Endpoint%20(LPC%202026).pdf
- 页码引用：p.6, p.7, p.8, p.9, p.11, p.12, p.13, p.14, p.15, p.17, p.18, p.19, p.20, p.21, p.23, p.24, p.25, p.26

## 跨议题综述

以下主题横跨多个议题，基于上文各节事实与分析综合而成，标注为分析。

### 主题一：信任边界的分割与代价

议题 1 与议题 3 都要求把资源分配与翻译控制的归属说清楚，但信任模型不同。Secure vIOMMU 将不可信 Hypervisor 排除出安全配置路径，由 ASP 管理安全设备表；MSHV external attach 则表达由外部 Hypervisor 管理 S2 的架构，不是在宣称 Hypervisor 不可信。IOMMUFD 需要显式表示这些不同的控制权和生命周期关系，而不能假设所有物理页表都由 Linux 自管。后续接口设计应把授权、对象引用、内存 pin 与翻译更新责任分别定义清楚。

### 主题二：可观测性是优化的前提

议题 2 的页表计数和 fdinfo 用于回答谁占用内存，议题 4 的 PMU 用于解释吞吐变化是否来自翻译压力下降。两者启示后续开发应同时交付机制和观测方法：回收方案需要归属、回收量和热路径开销数据；CONT 优化需要覆盖率、页表遍历和吞吐的关联验证。约 60% 的特定负载结果不应替代跨平台数据，也不能推断会议形成了统一的验收政策。

### 主题三：规格提示与实现契约的落差

议题 4 的 CONT 是硬件提示而非保证，议题 7 的 Type 3 ECN 是草案而非批准规范。两者都揭示了规格意图与实现现实之间的落差：CONT 不保证 TLB 合并，ECN 草案不提供正向条目保证。软件层必须以保守假设对待规格提示，在硬件自由度范围内设计正确性不依赖提示实现的路径。SMMU PMU 和 HMAT 验证要求分别是对这种落差的回应：用实测归因验证提示效果，用测试用例验证矩阵语义。

### 主题四：生命周期管理是跨议题瓶颈

议题 2 的 IOTLB 失效、议题 3 的 vm_file/vDEVICE 生命周期、议题 5 的 ST 目标生命周期，以及议题 6 的 reset-set 快照，都卡在对象生命周期与并发操作的对齐上。SRCU 保护软件侧但不保护硬件缓存；外部附加需对齐分区与设备生命周期；ST 条目在 DMABUF 无效后陈旧；总线复位期间设备可能被移除。这些问题的共同根源是跨组件或跨硬件/软件的对象生命周期缺乏统一的失效与同步契约，且每个议题都在各自层面提出了局部解法但未形成通用模式。

### 主题五：分层验证决定上游化质量

议题 5 的硬件无关 selftest 计划、议题 6 的机群验证能力、议题 8 的 QEMU 测试模型，为上游验证提供互补路径。接口自测适合常规回归，仿真适合低成本构造软件场景，多拓扑实机与机群测试则验证恢复、隔离和硬件差异。后续应公开覆盖矩阵、限制和复现方法，把“本机有效”推进为“在明确条件下可重复验证”，同时为未覆盖的生命周期事件保留实机验证。

## 术语表

- HWPT：IOMMUFD 硬件页表对象（Hardware Page Table），关联硬件翻译域；本文的 HWPT_EXTERNAL 是外部管理翻译域的提案对象，不表示页表由 IOMMUFD 直接管理。
- IOAS：IOMMUFD I/O 地址空间（I/O Address Space），是 IOMMUFD 的核心抽象，将设备、页表和映射关联在一起。
- IOVA：I/O 虚拟地址（I/O Virtual Address），设备发起 DMA 时使用的地址，由 IOMMU 翻译为物理地址。
- IOPT：I/O 页表（I/O Page Table），IOMMU 用于翻译 IOVA 到 PA 的多级页表结构。
- S1S2：两级翻译，stage-1（S1）为客户/进程页表，stage-2（S2）为宿主/Hypervisor 页表，二者嵌套形成完整翻译。
- ATS：地址翻译服务（Address Translation Services），PCIe 设备缓存翻译结果以减少 IOMMU 查找的机制。
- PASID：进程地址空间标识（Process Address Space ID），使单个设备支持多进程地址空间区分的标识符。
- DPC：下游端口遏制（Downstream Port Containment），PCIe 错误隔离机制，在相关端口限制故障流量的传播。
- AER：高级错误报告（Advanced Error Reporting），PCIe 错误报告与恢复机制。
- MPS：最大载荷大小（Max Payload Size），设备配置的 PCIe TLP 最大载荷；需满足相关接收端和通信路径约束，并非所有拓扑的寄存器值必须完全相同。
- MPSS：最大载荷大小支持（Max Payload Size Supported），PCIe 端点报告的 MPS 能力上限。
- TPH：TLP 处理提示（TLP Processing Hints），PCIe 机制，允许设备用提示标记 DMA 流量以优化缓存路由。
- PH：处理提示（Processing Hint），TPH 的组件，指示流量类别（如低延迟、大吞吐），不含具体缓存目标。
- ST：导向标签（Steering Tag），TPH 的组件，供接收方选择处理位置或缓存资源；具体含义由平台或设备定义，不能一概等同于 NUMA 节点编号。
- HMAT：异构内存属性表（Heterogeneous Memory Attribute Table），ACPI 表，描述发起方到目标方的延迟与带宽矩阵。
- EPC：端点控制器（Endpoint Controller），PCI Endpoint Framework 中表示 PCI 控制器的软件抽象。
- EPF：端点功能（Endpoint Function），PCI Endpoint Framework 中表示端点设备的软件抽象，绑定到 EPC 上运行。
