# PCIe 知识体系：从入门到 ATS、ACS 与设备虚拟化

> 面向第一次系统学习 PCIe 的读者。目标是建立完整知识地图，能解释设备发现、地址访问、DMA、中断、性能、隔离和故障之间的关系。
>
> “全量”在这里指覆盖主要知识领域与常见特性，并不等同于逐条翻译全部 PCI-SIG 规范。寄存器位定义、报文编码和精确时序，应查对应版本的 Base Specification、扩展规范和设备手册。特性存在于规范中，不代表某台机器、某个端口或某个驱动支持它。
>
> 软件示例以 Linux 为主；源码链接固定在 Linux v6.6，便于复核。docs.kernel.org 的文档可能随主线演进，使用接口时应与运行内核版本对照。本文命令是学习示例，未在读者机器上执行。
>
> 资料整理日期：2026-11-11。规范发布状态与设备实际部署状态分别判断。

## 目录与阅读方式

1. [先建立整体模型](#1-先建立整体模型)
2. [拓扑、链路与代际](#2-拓扑链路与代际)
3. [分层、报文与传输可靠性](#3-分层报文与传输可靠性)
4. [设备发现、配置空间和资源分配](#4-设备发现配置空间和资源分配)
5. [地址空间、MMIO、DMA 和中断](#5-地址空间mmiodma-和中断)
6. [排序、一致性与性能](#6-排序一致性与性能)
7. [IOMMU：隔离和地址转换的基础](#7-iommu隔离和地址转换的基础)
8. [ATS：让设备缓存地址翻译](#8-ats让设备缓存地址翻译)
9. [PASID、PRI 和 SVA](#9-pasidpri-和-sva)
10. [ACS：控制流量路径与隔离边界](#10-acs控制流量路径与隔离边界)
11. [SR-IOV、直通与 IOMMU Group](#11-sr-iov直通与-iommu-group)
12. [电源管理、复位、热插拔和错误恢复](#12-电源管理复位热插拔和错误恢复)
13. [其他重要特性地图](#13-其他重要特性地图)
14. [Linux 观察与排障练习](#14-linux-观察与排障练习)
15. [学习路线、检查题与术语表](#15-学习路线检查题与术语表)
16. [参考资料与源码入口](#16-参考资料与源码入口)

第一次阅读：1 → 2 → 4 → 5 → 7 → 8 → 9 → 10 → 11。第二遍补报文、性能与恢复。学习 ATS 和 ACS 前，至少弄懂“设备发起 DMA”“IOVA”“Requester ID”和“交换机本地转发”这四件事。

## 1. 先建立整体模型

### 1.1 PCIe 解决什么问题

PCI Express，简称 PCIe，是主机与网卡、SSD、GPU、加速器等设备通信的互连体系。它定义的不只是电气信号，还包括报文、设备发现、配置、资源访问、错误处理等规则。

传统 PCI 常用共享并行总线；PCIe 使用点到点串行链路，多个设备通过交换结构组成层级拓扑。软件侧保留了大量 PCI 的配置与驱动模型，所以 Linux 中相关目录、API 和命令通常仍叫 `pci`。

先把系统拆成三个方向：

| 方向 | 例子 | 首先要理解的问题 |
|---|---|---|
| CPU 访问设备 | 驱动写网卡控制寄存器 | 哪段地址属于设备？如何保证写入顺序？ |
| 设备访问内存 | 网卡把收到的数据写入内存 | 使用什么地址？谁授权？何时对 CPU 可见？ |
| 设备通知 CPU | NVMe 队列完成后发中断 | 中断如何路由到对应 CPU？ |

PCIe 是运输与访问基础设施。NVMe 定义存储控制器协议，网卡有自己的队列协议，GPU 有自己的命令接口。这些设备可共同使用 PCIe，但驱动的工作流程并不相同。

### 1.2 必须分开的五个概念

1. **发现设备**：读配置空间，知道设备是谁。
2. **分配资源**：给 BAR、桥窗口和中断等资源安排位置。
3. **搬运数据**：通过 MMIO、DMA 和设备队列完成工作。
4. **隔离设备**：控制它能访问哪些内存、哪些其他设备。
5. **管理生命周期**：处理启动、休眠、复位、故障和移除。

`lspci` 能看到设备，只能证明配置访问已经达到一定程度；不证明 DMA、中断、ATS、虚拟化或业务负载可用。

## 2. 拓扑、链路与代际

### 2.1 典型拓扑

```text
CPU / 内存控制器
       |
Root Complex（根复合体，连接主机地址体系与 PCIe）
       |
Root Port（根端口）
       |
Switch Upstream Port（交换机上行端口）
       |
       +-- Downstream Port -- NVMe Endpoint
       |
       +-- Downstream Port -- NIC Endpoint
```

- **Root Complex，RC**：连接 CPU、内存子系统和 PCIe 层级。一个 RC 可以有多个 Root Port；RC 不能简单等同于某一个插槽。
- **Root Port，RP**：根部面向下游链路的端口，在软件中通常体现为桥。
- **Endpoint，EP**：终端设备，如 NVMe 控制器、网卡、GPU。
- **Switch**：提供多个端口，按地址、ID 等规则转发事务；从软件视角看包含多个桥。
- **Bridge**：连接不同总线段；PCIe 还可能通过桥接芯片连接传统 PCI。
- **RCiEP / RCEC**：根复合体集成端点及关联事件收集器，常见于片上集成场景，不能套用外接卡的全部链路假设。

这里的 upstream/downstream 是相对根方向的拓扑描述，并不意味着数据只能单向传输。

### 2.2 Lane、Link、插槽和分叉

一个 lane 包含两个传输方向的差分信号通道：一个方向发送，另一个方向接收。链路可聚合为 x1、x2、x4、x8、x16 等宽度，支持全双工。

需要同时区分：

- 插槽的物理长度。
- 主板实际连接的 lane 数。
- 设备支持的最大宽度。
- 当前协商成功的速率和宽度。

一个外形为 x16 的插槽可能只接 x4。即使卡和插槽都支持 x16，接触不良、布线或训练问题也可能让链路只协商到 x8。

**Bifurcation（链路分叉）**是把控制器支持的一组 lane 配成多个独立链路，例如 x16 分成四个 x4。它需要控制器、主板布线和固件配合；无源转接板无法自己实现 PCIe Switch 的功能。

**Retimer**恢复和重新定时信号，改善长通道信号质量；它不是负责地址路由的交换机。**Redriver**主要进行信号调理，也不承担交换路由。

M.2、U.2、U.3、CEM 插卡、OCuLink 等是连接器、外形或布线生态中的不同概念。“使用 M.2 接口”本身不能证明设备走 PCIe，也不能证明它支持 NVMe。

### 2.3 版本和理论带宽

下表为每 lane、每方向的数量级。GB/s 使用十进制；x16 的单方向带宽再乘 16，双向合计再乘 2。实际业务吞吐还要扣除报文和协议开销。

| 代际 | 标称每 lane 速率 | 物理编码/传输特点 | 每 lane 每方向理论带宽参考 |
|---|---|---|---|
| Gen1 | 2.5 GT/s | NRZ，8b/10b | 0.250 GB/s，扣除编码后 |
| Gen2 | 5 GT/s | NRZ，8b/10b | 0.500 GB/s，扣除编码后 |
| Gen3 | 8 GT/s | NRZ，128b/130b | 约 0.985 GB/s，扣除编码后 |
| Gen4 | 16 GT/s | NRZ，128b/130b | 约 1.969 GB/s，扣除编码后 |
| Gen5 | 32 GT/s | NRZ，128b/130b | 约 3.938 GB/s，扣除编码后 |
| Gen6 | 64 GT/s | PAM4，FLIT、FEC | 8 GB/s 原始速率预算，尚有 FLIT/FEC 等开销 |
| Gen7 | 128 GT/s | PAM4，延续 FLIT/FEC 体系 | 16 GB/s 原始速率预算，尚有协议开销 |

Gen7 的规范发布与具体产品普及是两件事；选型应查看实际 CPU、交换机和设备支持情况。Gen6 x16 常见的 256 GB/s 和 Gen7 x16 的 512 GB/s 是双向合计口径，单方向原始预算分别为 128 GB/s、256 GB/s。[R1][R2][R18]

Gen3 x4 的编码后理论单方向带宽：

```text
8 x 10^9 transfer/s x (128/130) x 4 lane / 8 bit/byte
= 3.938 x 10^9 byte/s
≈ 3.94 GB/s
```

这还不是 SSD 必然达到的读写速度。TLP 头、DLLP、读请求与 Completion、流控、设备内部处理、介质和软件都可能限制吞吐。

GT/s 是传输速率指标，不能直接写成 GB/s。PAM4 每个符号承载两比特，因此也不能把 Gen6 的 64 GT/s 当成 64 Gbaud 再额外翻倍计算。

新旧代际通常可以协商兼容，但实际运行在共同支持的速率和宽度上。兼容不等于任何转接、供电、信号条件下都能正常工作。

## 3. 分层、报文与传输可靠性

### 3.1 三层分别负责什么

| 层 | 核心职责 | 典型概念 |
|---|---|---|
| Transaction Layer | 表达“访问什么、谁发起、谁完成” | TLP、地址、Tag、Requester ID、属性 |
| Data Link Layer | 相邻链路的可靠传输与流控相关机制 | 序号、链路校验、确认、重传 |
| Physical Layer | 把信息发送到实际链路 | lane、编码、均衡、训练、速率 |

协议在后续代际引入 FLIT 后，一些封装、校验和重传机制发生变化。不能把 Gen1–5 非 FLIT 模式的所有字段和时序直接套到 Gen6 的 FLIT 模式。[R2]

### 3.2 TLP 与 DLLP

**TLP（Transaction Layer Packet）**承载事务：Memory Read/Write、Configuration Read/Write、Completion、Message、AtomicOp 等。

**DLLP（Data Link Layer Packet）**用于链路管理，例如传统模式中的确认与流控更新。它不是另一个业务数据搬运协议。

TLP 常见信息包括类型、长度、地址或路由标识、Requester ID、Tag、字节使能及 Relaxed Ordering、No Snoop 等属性。是否有数据载荷、头部如何编码，取决于报文类型和工作模式。

### 3.3 Posted、Non-Posted 和 Completion

| 类别 | 例子 | 是否期望事务完成报文 |
|---|---|---|
| Posted Request | 常见 Memory Write | 不返回对应的成功 Completion |
| Non-Posted Request | Memory Read、配置读写 | 请求后需要 Completion |
| Completion | 返回读出的数据或状态 | 响应前面的请求 |

注意：配置写也需要 Completion；不能把所有“写”都理解为 Posted。

```text
一次设备读取主机内存：
Device -- Memory Read(Requester ID, Tag, Address) --> Host
Device <-- Completion with Data(Tag, Data) -------- Host

一次设备写入主机内存：
Device -- Memory Write(Address, Data) ------------> Host
         没有与这次写一一对应的成功 Completion
```

“没有 Completion”不代表没有链路级可靠性，也不代表软件已经确认数据到达最终使用者。链路确认、事务完成和设备业务完成是不同层面的事件。

读请求可以拆成多个 Completion 返回。Requester 使用 Tag 等信息关联请求与响应；Tag 和可用 outstanding 请求数会影响读吞吐。

### 3.4 路由方式

- **地址路由**：典型 Memory 请求，依据目标地址和桥窗口转发。
- **ID 路由**：例如 Completion 回到发起者，依靠目标 ID 定位。
- **隐式路由**：某些 Message 按规范定义的方向或范围传播。

交换机不会对每个包都向 CPU 软件询问下一跳。枚举阶段配置的桥窗口、总线号与端口控制决定硬件如何转发。

### 3.5 流控与重传

发送端不能无限向接收端灌包。接收缓冲区通过 credit 机制约束可发送的报文数量或数据量；Posted、Non-Posted、Completion 的资源分类有助于避免相互挤占导致协议死锁。

传统链路层用序号、LCRC、ACK/NAK 和 replay buffer 处理链路传输错误。**Replay 是链路级重发，不是驱动重新提交一次业务请求。** ECRC 则是可选的端到端校验机制，作用范围与 LCRC 不同。

Gen6 引入低延迟 FEC 与 FLIT 相关 CRC、重传机制。FEC 用于纠正一定范围的传输错误，不等于链路永远不会报错或无需重传。

**FLIT（Flow Control Unit）**是固定长度的传输组织单位，并不等于一条 TLP。Gen6/Gen7 使用的 256 字节 FLIT 中，236 字节用于 TLP 区域、6 字节用于 DLP、8 字节用于 CRC、6 字节用于 FEC。TLP 可以在 FLIT 中组织或跨越 FLIT；236 字节区域中还含 TLP 头等信息，所以 `236/256` 也不是最终应用数据效率。[R18]

### 3.6 LTSSM

LTSSM 是 Link Training and Status State Machine，链路训练与状态机。常见状态包括 Detect、Polling、Configuration、L0、Recovery、Disabled、Hot Reset，以及不同节能状态。

- **L0**：正常活动状态。
- **Recovery**：恢复、速率切换或重新训练相关过程，不必然意味着硬件已损坏。
- **Equalization**：高速链路的均衡训练，信号质量不够可能导致降速或训练失败。

`lspci` 显示的当前速度与宽度是观察结果；无法仅凭它还原训练失败的全部原因，深入分析可能需要控制器调试寄存器或协议分析仪。

## 4. 设备发现、配置空间和资源分配

### 4.1 Segment、BDF 和 Requester ID

Linux 常见设备地址：`0000:03:00.0`。

```text
0000 : 03 : 00 . 0
域/段   Bus  Device Function
```

传统 BDF 中 Bus 为 8 位，Device 为 5 位，Function 为 3 位；Requester ID 通常是 16 位 BDF 编码。**Segment 不包含在这 16 位 Requester ID 中**，平台还需要根据 PCI segment、IOMMU 实例等上下文区分地址域。

Function 是软件可独立发现和管理的功能实体。一张物理网卡可能有多个 Function，一个 Function 也可能提供很多队列。

**ARI（Alternative Routing-ID Interpretation）**改变 Device/Function 字段的解释方式，允许在相应层级表达更多 Function，常与 SR-IOV 一起出现；它不会把一个物理设备自动变成多个独立可信实体。

### 4.2 配置空间

PCI 传统配置空间为每 Function 256 字节；PCIe 扩展配置空间可到 4 KiB。配置空间用于设备识别、资源描述和控制，不是普通 DMA 数据缓冲区。

重要内容包括：

- Vendor ID / Device ID、Subsystem ID、Class Code。
- Command / Status，例如 Memory Space Enable、Bus Master Enable。
- BAR、桥总线号与地址窗口。
- Interrupt 相关字段。
- Capability 与 Extended Capability 链表。

普通 Capability 中可出现 PM、MSI、PCI Express、MSI-X；扩展 Capability 中可出现 AER、ACS、ARI、ATS、SR-IOV、PRI、PASID 等。它们有不同的链表格式和位置，不能简单连续扫描固定偏移。

### 4.3 ECAM

ECAM 是 Enhanced Configuration Access Mechanism，把 PCIe 配置访问组织到 CPU 可访问的地址窗口。常见布局按 Bus 1 MiB、Device 32 KiB、Function 4 KiB 计算偏移，具体基址、有效总线范围由平台描述。

在 ACPI 平台常见 MCFG 描述，在设备树平台则有 host bridge 的相应描述。ECAM 的 CPU 地址是主机访问配置窗口的入口，不能把它当成设备的数据 BAR。

### 4.4 枚举过程

概念上可以理解为：

```text
固件/内核识别 Host Bridge
  -> 扫描总线上的 Function，读取 Vendor/Device ID
  -> 发现桥，确定 Primary/Secondary/Subordinate Bus
  -> 递归扫描下游
  -> 发现资源需求，分配/接纳 BAR 与桥窗口
  -> 按 ID / Class 匹配驱动
  -> 驱动初始化设备、DMA 与中断
```

真实系统可能大量沿用固件已经分配的资源，也可能需要重新分配。热插拔还涉及为未来设备预留总线号和地址窗口。

### 4.5 BAR 到底是什么

BAR（Base Address Register）描述一个 Function 暴露的地址资源。CPU 访问被映射到这段资源的地址时，请求最终到达设备。

- Memory BAR：可表示寄存器、设备内存、队列窗口等。
- I/O BAR：保留了 I/O 空间模型，现代高速设备常以 Memory BAR 为主。
- 64 位 Memory BAR 使用两个相邻 BAR 槽位，因此“六个 BAR 槽位”不等于“六块独立的 64 位区域”。
- Prefetchable 属性是资源语义声明；它不等于 CPU 可以随意把设备寄存器当普通可缓存内存。

BAR 的容量探测由 PCI 核心在合适的生命周期内进行。学习时不要对正在运行的设备手工写全 1 来探测 BAR，这会破坏现有资源配置。

**Resizable BAR**允许在硬件支持的尺寸集合中选择 BAR 大小。它扩大可映射窗口，不会增加 PCIe lane，也不保证任意业务自动加速。

### 4.6 CPU 地址和 PCI 总线地址

Host Bridge 可能把 CPU 物理地址窗口转换为不同的 PCI 总线地址。因此：

```text
CPU 物理地址窗口 -- Host Bridge 地址映射 --> PCI 总线地址 -- BAR 匹配 --> 设备
```

在某些平台两者数值相等，只是一种平台安排。驱动应使用 PCI 和 DMA API，不能依靠“在这台 x86 机器上数值一样”的经验跨平台编程。[R3][R4]

## 5. 地址空间、MMIO、DMA 和中断

### 5.1 先认清地址属于谁

| 地址 | 谁使用 | 通常由谁解释 |
|---|---|---|
| CPU 虚拟地址 VA | 用户进程、内核代码 | CPU MMU |
| CPU 物理地址 PA | 主机内存体系 | 内存控制器及主机互连 |
| PCI 总线地址 | PCIe Memory 请求 | 桥窗口、BAR、平台映射 |
| DMA 地址 / IOVA | 发起 DMA 的设备 | IOMMU 或平台 DMA 地址映射 |
| 设备内部地址 | GPU 显存、设备 SRAM 等 | 设备自己的内部逻辑 |

IOVA 是 I/O Virtual Address。使用 IOMMU 的常见情形中，设备发出 IOVA，经 IOMMU 转为可访问的物理目标；没有 IOMMU 时，DMA 地址仍然不能普遍等同于 CPU 虚拟地址或直接由 `virt_to_phys()` 推得。[R4]

### 5.2 MMIO：CPU 访问设备

```text
驱动中的映射地址
  -> CPU 页表转换到设备资源的物理窗口
  -> Root Complex 生成 Memory Read/Write
  -> 经过交换机
  -> 命中设备 BAR
  -> 设备执行寄存器或窗口访问
```

Linux 驱动通常通过 `pci_iomap()` / 相关资源映射接口获取 `__iomem` 映射，使用 `readl()`、`writel()` 等访问器。设备寄存器的副作用、访问宽度、字节序和顺序约束必须按设备与架构契约处理。

一次 `writel()` 返回，不保证写已经到达设备内部的最终执行点。需要刷新 Posted Write 时，常用读取合适的设备寄存器等方法，具体遵守 PCI 与设备文档。[R5]

### 5.3 DMA：设备主动访问内存

下面用网卡发送说明控制与数据如何分离：

```text
CPU 准备数据和描述符
  -> DMA API 为缓冲区建立设备可用映射
  -> 描述符写入 DMA 地址、长度等信息
  -> 按要求保证描述符对设备可见
  -> CPU 用 MMIO 写 doorbell
  -> 网卡发 Memory Read 取描述符和数据
  -> 网卡完成发送，DMA 写回完成状态
  -> 网卡发 MSI-X 或驱动轮询完成队列
  -> 驱动按生命周期回收/解除映射
```

Doorbell 是通知设备“有新任务”的寄存器或机制，不承载全部业务数据。DMA 引擎按队列协议搬运数据，PCIe 负责运输相关事务。

### 5.4 Linux DMA API 应掌握的边界

- `dma_set_mask_and_coherent()`：声明设备能够使用的 DMA 地址范围；设备地址宽度不由 C 指针宽度决定。
- `dma_alloc_coherent()`：分配适合 CPU 与设备共享、满足相应一致性契约的内存。
- `dma_map_single()` / `dma_map_sg()`：建立流式 DMA 映射，返回设备应使用的地址。
- `dma_sync_*_for_cpu()` / `dma_sync_*_for_device()`：在需要时进行所有权切换和缓存同步。
- `dma_unmap_*()`：结束映射；必须确保设备不会继续访问相应区域。

DMA 方向以设备视角命名：`DMA_TO_DEVICE` 是内存到设备，`DMA_FROM_DEVICE` 是设备到内存。

Coherent 不等于所有操作自动按程序期望的顺序发生。描述符内容、有效位和 doorbell 之间仍可能需要 DMA 内存屏障及适当 MMIO 访问语义。[R4][R5]

SWIOTLB 等机制还可能通过 bounce buffer 处理 DMA 可达性等限制。因此 DMA 地址翻译不只包含“开了/没开 IOMMU”两种简单状态。

### 5.5 中断：INTx、MSI 与 MSI-X

| 方式 | 工作方式 | 学习重点 |
|---|---|---|
| INTx | 兼容传统引脚中断语义，PCIe 中通过消息表达相应事件 | 共享、路由和电平语义 |
| MSI | 设备向特定地址写入特定数据触发中断 | 本质为 Memory Write，可分配有限向量 |
| MSI-X | 更灵活的向量表和逐向量控制，表/PBA 通常位于 BAR 指定位置 | 多队列设备常用，最多可描述 2048 个表项 |

MSI-X 表容量只是硬件上限；实际可分配向量数受中断控制器、内核、CPU 和驱动策略限制。

```text
设备业务完成
 -> 写完成队列/状态
 -> 发 MSI-X Memory Write
 -> 主机中断控制与可选中断重映射
 -> 目标 CPU 的中断处理程序
 -> 驱动消费完成队列
```

设备向内存写数据和向 CPU 发中断的可见性关系需要遵守 PCIe 排序及驱动 API 契约。CPU 收到中断后，也不能忽略 DMA API 对非一致性平台的要求。

MSI/MSI-X 是通知机制；IOMMU 的 DMA 地址转换与**中断重映射**是不同功能。安全直通常需要同时考虑两类请求。[R6]

## 6. 排序、一致性与性能

### 6.1 三种“有序”不要混淆

1. CPU 编译器/执行顺序：涉及编译器屏障和 CPU 内存模型。
2. CPU 与设备共享内存的可见性：涉及缓存一致性、DMA API 和内存屏障。
3. PCIe 事务之间的允许排序：涉及 Posted/Non-Posted、Traffic Class 和事务属性。

PCIe 有明确的排序规则，但不是“所有设备、所有报文全局严格串行”。驱动不能用一句“PCIe 保序”替代对并发和可见性的分析。

**Relaxed Ordering（RO）**允许符合规范的事务放宽部分排序约束，可能改善吞吐；它不是允许任意乱序。**ID-Based Ordering（IDO）**进一步利用请求身份放宽相应约束，仍受支持条件和规则限制。

**No Snoop**表达事务对主机缓存侦听处理的属性。平台可能控制或覆盖其行为；它不是通用的“安全提高性能”开关，错误使用会破坏数据可见性假设。

### 6.2 MPS 与 MRRS

- **MPS，Max Payload Size**：一次相关 TLP 可携带的最大数据载荷设置。路径与各组件能力会限制可用值。
- **MRRS，Max Read Request Size**：一个读请求允许请求的数据量上限。
- **Read Completion Boundary、Completion 拆分**：还会影响返回数据如何分包。

增大 MRRS 不等于每个 Completion 都变成同样大的包；增大 MPS 也不等于读请求的所有限制自动消失。不要脱离整条路径的支持条件强行设置最大值。

### 6.3 为什么链路没有跑满

常见限制包括：

| 现象 | 可疑瓶颈 |
|---|---|
| 设备标称 Gen4 x16，实际吞吐接近 Gen3 x8 | 当前训练速率/宽度、拓扑共享上行 |
| 大块写较快、小块读较慢 | 请求往返时延、Tag/outstanding 不足、包头比例 |
| 设备很快但 CPU 占用高 | 中断过密、批处理不足、队列和软件路径 |
| 跨 NUMA 节点性能明显下降 | 内存位置、CPU 亲和性、跨 socket 互连 |
| 开启 IOMMU 后特定负载下降 | 映射成本、IOTLB miss、失效开销、页粒度 |
| 同交换机两卡同时工作变慢 | 共享上行口带宽或交换机内部资源 |

用一个简化上界理解 outstanding 的作用：

```text
可维持读吞吐 ≲ 在途有效数据量 / 往返时延

如果在途只有 64 KiB，往返时延是 2 us：
64 KiB / 2 us ≈ 32.8 GB/s
```

这只是估算模型，尚未计入链路带宽、请求开销、Completion 调度和设备处理。不要把示例数字当作某台硬件的实测。

### 6.4 QoS 与服务质量

Traffic Class（TC）用于标记业务类别，Virtual Channel（VC）提供相应的虚拟通道和流控资源组织。常见系统只实际使用默认通道；仅设置不同 TC 不一定获得独立资源或端到端 QoS。

TPH、设备中断亲和性、NUMA 布局和队列划分可以帮助数据靠近处理 CPU，但“提示”不保证硬件一定按期望放置数据。

## 7. IOMMU：隔离和地址转换的基础

### 7.1 MMU 与 IOMMU

CPU MMU 服务于 CPU 发起的内存访问；IOMMU 服务于受其管理的设备请求。IOMMU 常见职责包括 DMA 重映射、权限检查、错误报告；平台也可能提供相应中断重映射能力。

```text
普通 DMA：
设备 -- (Requester ID, IOVA, 读/写) --> IOMMU
     -- 根据身份找到 domain / 页表
     -- 检查权限、翻译
     --> 主机物理内存
```

**Domain**可理解为一组 DMA 翻译与权限上下文；一个 Domain 可以管理多个设备。**IOTLB**是 IOMMU 一侧的翻译缓存，后文的 **ATC** 则位于设备一侧。

### 7.2 为什么只有页表还不够

即使 IOMMU 页表配置正确，也必须保证所有需要隔离的请求确实经过受控路径。如果两个设备在交换机下直接 P2P 转发，请求可能根本不经过负责主机内存隔离的 IOMMU。

另外还要处理：

- 设备身份是否可信，是否存在 DMA alias。
- 请求是否被错误地标记为已经翻译。
- 中断是否能被伪造或送错目标。
- 设备复位、卸载时是否仍持有有效 DMA 能力。
- 驱动释放内存后，旧翻译和在途请求是否清理完毕。

ATS 管翻译缓存协作，ACS 管部分路径与访问控制，PASID 管更细的地址空间身份，PRI 管缺页请求。这几项各有职责，不能互相代替。

## 8. ATS：让设备缓存地址翻译

### 8.1 ATS 解决什么问题

ATS 是 **Address Translation Services，地址翻译服务**。没有 ATS 时，普通受重映射的 DMA 请求通常到达 IOMMU 后再查翻译；高频随机访问可能受到 IOTLB 容量、页表遍历和访问时延影响。

ATS 允许支持它的设备向 Translation Agent（常见为主机 IOMMU 相关单元）请求翻译，把结果缓存到设备端 **ATC，Address Translation Cache** 中，再使用相应的已翻译请求。

它不是“把 CPU 页表直接发给所有设备”，也不是“让设备未经授权访问任意物理地址”。是否允许 ATS、使用哪个上下文、返回什么权限，都由平台和软件管理。[R7][R8]

### 8.2 普通 DMA 与 ATS 的对照

```text
不使用 ATS：
Device -- Untranslated DMA(IOVA) --> IOMMU 查翻译/权限 --> 内存

使用 ATS，ATC 未命中：
Device -- Translation Request(IOVA, 身份, 所需访问) --> Translation Agent
Device <-- Translation Completion(翻译结果/权限等) -- Translation Agent
Device 缓存结果到 ATC
Device -- Translated Memory Request(已翻译地址) ----> 主机内存路径

后续 ATC 命中：
Device -- Translated Memory Request ----------------> 主机内存路径
```

Translation Completion 返回的是地址翻译相关结果，不是业务数据。拿到翻译之后，设备仍需另发 Memory 请求读取或写入业务数据。

TLP 的 Address Type（AT）语义用于区分未翻译请求、翻译请求和已翻译请求等情况；精确编码和允许组合应查对应规范。PASID 等前缀/上下文可以参与翻译，但不是所有 ATS 使用场景都必须使用进程级 SVA。

### 8.3 ATC 缓存的不只是一个数字

理解 ATC 至少要包含：

- 与哪个地址空间/上下文相关。
- 被翻译的地址范围及页粒度。
- 翻译后的目标地址。
- 允许的访问类型等权限语义。

设备不能在只获得读权限后，把同一结果当作写权限使用。使用 PASID 时，也不能把进程 A 的缓存条目当作进程 B 的翻译。

“设备缓存了物理地址”只是帮助入门的简化说法。在虚拟化、多阶段翻译等环境中，Translation Agent 提供的结果及后续处理取决于平台架构，不能固定画成所有系统都完全相同的单级直达路径。

### 8.4 失效：ATS 最重要的正确性问题

假设一个 IOVA 原先映射到物理页 P1，后来操作系统准备回收 P1 并改映射到 P2。只修改内存中的页表是不够的：IOMMU 和设备 ATC 中都可能还保留旧结果。

```text
概念上的回收闭环：
OS 决定撤销/修改映射
 -> 按平台契约阻止不合适的新访问并更新翻译状态
 -> 清理相关 IOMMU 翻译缓存
 -> 向设备发 ATS Invalidate Request
 -> 设备按协议清理匹配 ATC 条目，处理相关请求顺序约束
 -> 返回 Invalidate Completion
 -> 软件在全部必要失效和请求排空条件满足后安全复用内存
```

这个图表达必要条件，不规定所有 IOMMU 驱动都采用同一条固定调用顺序。精确的同步、排空和失效顺序必须遵守平台规范。

只清 IOMMU IOTLB，不清设备 ATC，会留下陈旧翻译。只禁用 ATS，也不能自动证明所有在途 DMA 已经结束。设备卸载、进程退出、权限撤销、迁移和虚拟机重绑定都要覆盖相应生命周期。

### 8.5 支持与启用条件

一般需要：

1. 设备提供 ATS Extended Capability，硬件正确实现协议。
2. Root Complex、IOMMU 和路径支持对应请求。
3. 固件和 OS 能够描述并管理相关能力。
4. 驱动/IOMMU 子系统按安全策略启用，并协调失效。
5. 与 ACS 等路径控制的配置相容。

Linux v6.6 可从 `drivers/pci/ats.c` 中的 `pci_ats_supported()`、`pci_enable_ats()`、`pci_disable_ats()`、`pci_ats_queue_depth()` 等入口学习。它们是 PCI 能力管理入口，不是独立完整的 SVA 实现。[R8]

其中 `pci_ats_supported()` 会拒绝被标记为 `untrusted` 的设备。这是 Linux 对设备信任的具体策略，不是说 ATS Capability 在硬件上消失了。`pci_ats_queue_depth()` 返回的是失效请求队列相关能力，不能理解成业务 DMA 队列深度或 ATC 条目总数。[R8]

`lspci -vv` 中的 `ATSCap` / `ATSCtl` 能辅助观察能力与控制状态，但仅看到 ATS Capability 不代表已经启用，更不代表某个应用真的在使用。

### 8.6 收益与局限

可能收益：降低重复地址翻译成本，缓解 IOMMU 翻译缓存压力，配合进程地址空间访问。

代价与限制：ATC 容量有限；miss 仍需要翻译往返；映射频繁变化会增加失效成本；多设备、多进程和大规模地址空间带来管理开销。收益高度依赖工作集、页大小、地址重用与平台实现。

安全上，主机必须能够信任设备正确遵守翻译结果和失效协议。ATS 的启用是隔离设计的一部分；不能简单宣称“用了 ATS 后每个已翻译请求仍进行与未翻译请求完全相同的逐次页表权限检查”。

## 9. PASID、PRI 和 SVA

### 9.1 为什么 Requester ID 不够

只用 RID，主机主要知道“哪个 Function 发来的请求”。如果同一个 GPU 同时为多个进程工作，还需要知道“它正在代表哪个地址空间访问”。

**PASID，Process Address Space ID**为请求提供额外地址空间标识。它不是 Linux PID：数值不必相同，分配与绑定生命周期也不同。

```text
RID                  -> 识别设备 Function
RID + PASID          -> 进一步选择该设备使用的地址空间上下文
访问类型/权限属性    -> 决定允许读、写及其他访问语义
```

PASID 的协议字段最多可表达 20 位标识，但设备和平台支持宽度可能更小。它并不天然赋予任意权限；系统必须先建立标识与地址空间、访问权限之间的绑定。[R7][R8]

### 9.2 SVA 与 SVM

**SVA，Shared Virtual Addressing**让设备能够使用与某个进程关联的虚拟地址空间。例如应用把一个用户态指针交给加速器，设备在正确绑定的上下文中解释这个地址。

SVM（Shared Virtual Memory）在不同厂商生态中含义和范围可能略有差异。学习时先问清：是否共享虚拟地址？是否支持缺页？是否允许按需迁移？是否缓存一致？这些不是同一个问题。

SVA 不意味着 CPU 与设备自动具备所有数据缓存一致性，也不意味着设备可以忽略进程权限。CPU 页表、IOMMU 页表/翻译机制、PASID 与设备队列上下文需要协同。[R7]

### 9.3 PRI：设备发现页暂时不可用时怎么办

**PRI，Page Request Interface**允许设备为缺页或其他可恢复的地址可用性问题请求操作系统介入。它解决的是“页还没准备好，能否请求准备”，不是替代 DMA 数据通道。

```text
进程提交任务，设备使用 VA + PASID
 -> 设备需要翻译，或发现对应页尚不可访问
 -> 设备发 Page Request，携带相关身份/访问需求
 -> 平台把请求交给 OS 的 I/O page fault 处理机制
 -> OS 检查上下文与权限，尝试使页可用/建立映射
 -> 返回 Page Response：成功或失败等结果
 -> 成功后设备按协议重试翻译/访问
 -> 失败后设备结束相应任务或报告错误
```

设备不会仅凭“发了请求”就保证缺页成功处理。地址可能非法、权限不足、进程可能退出，资源也可能不足。

PRI 使用 Page Request Group 等机制组织请求和响应，支持 outstanding 请求数量也有限。软件必须处理分组完成、限流和错误；不能把每一条请求都想成独立、无限并发的 CPU 缺页异常。

### 9.4 ATS、PASID、PRI 各自做什么

| 能力 | 回答的问题 | 不负责什么 |
|---|---|---|
| ATS | 设备如何取得并缓存授权翻译？ | 不负责创建进程，不保证按需缺页成功 |
| PASID | 这个请求属于哪个地址空间上下文？ | 不自行完成地址翻译，不等于 Linux PID |
| PRI | 地址暂时不可用时，如何请求主机处理？ | 不返回业务数据，不自动授予权限 |
| IOMMU | 如何基于身份执行翻译与访问控制？ | 无法单独阻止所有绕过它的 P2P 路径 |
| SVA | 如何把进程地址空间提供给设备使用？ | 不自动提供所有缓存一致性语义 |

三项经常一起用于按需分页的加速器，但不是任何场景都必须同时启用。例如 ATS 可以服务非进程级的 DMA 翻译；固定驻留、预先映射的地址空间也不一定依赖 PRI。实际 SVA 支持组合受 CPU 架构、IOMMU、设备和驱动约束。

### 9.5 生命周期比启动更难

建立绑定只是开始，还必须考虑：

- 进程 `munmap()`、权限变化、页面迁移。
- 设备 ATC 与 IOMMU 缓存失效。
- 任务取消后，是否还有未完成的 DMA/PRI 请求。
- 进程退出时，如何解绑 PASID 并清理设备队列。
- PASID 被重新分配前，旧请求是否可能误命中新上下文。
- 设备复位后，上下文和能力状态是否需要重新建立。

Linux 常通过 MMU notifier 等机制连接 CPU 地址空间变化与设备/IOMMU 管理。具体阻塞、失效和解绑策略应阅读对应驱动，不能只凭一个 PASID Capability 判断完整支持。[R7]

### 9.6 Linux v6.6 中可以继续追踪什么

`drivers/pci/ats.c` 同时包含 PRI 和 PASID 的能力管理：[R8]

- `pci_enable_pri()` 会检查能力和停止状态，并结合设备最大请求数配置请求额度。它没有代替 IOMMU fault handler 执行页面调入。
- `pci_enable_pasid()` 检查 PASID 能力、所请求的附加能力、端到端前缀路径条件，以及相应 ACS 路径条件。
- `pci_max_pasids()` 用来了解设备可支持的 PASID 规模，但不意味着系统已经为所有这些标识创建了上下文。

这些检查体现特定 Linux 版本的实现契约；协议层面，PASID 能力与 ATS/PRI 能力是独立的。不要把“某个 Linux API 还有这些前置条件”写成“PCIe 所有平台永远必须采用同一组合”。[R19]

## 10. ACS：控制流量路径与隔离边界

### 10.1 ACS 解决什么问题

**ACS，Access Control Services**是一组访问控制与路由控制机制，用于提升 PCIe 层级中请求路径和身份检查的可控性，尤其影响 P2P、设备直通和多租户隔离。

考虑两个设备位于同一个交换机下：

```text
                 Root / IOMMU
                       |
                    Switch
                    /    \
                   A      B

允许本地 P2P：A ----------> B
强制向根方向重定向：A --> Switch --> Root 方向 --> 后续由平台处理
```

本地 P2P 请求可能不经过预期的主机侧 IOMMU。ACS 的相应重定向控制可以改变这一点，但**上行重定向不保证平台一定支持再路由回目标设备**。Root Complex 的 P2P 支持和软件策略仍是独立条件。[R9][R10]

### 10.2 七类 ACS 控制逐项解释

不同组件类型适用和必须实现的控制并不相同，应结合端口类型、Capability 和规范要求判断。以下是常见 ACS Capability/Control 中的七类功能，不是说所有 Function 都必须具备七项。

| 缩写 | 名称 | 作用与需要注意的边界 |
|---|---|---|
| SV | Source Validation | 检查经过相应端口的请求源身份是否符合拓扑范围，降低源身份伪造风险；不等同于内容认证或加密 |
| TB | Translation Blocking | 对适用的上行请求阻断 Translation Request 或已翻译请求等 AT 非零情况，防止未经允许使用翻译服务/已翻译路径；与 ATS 配置必须协调 |
| RR | P2P Request Redirect | 将适用的 P2P 请求重定向到上游，避免直接在本地向对端转发；不保证上游能完成该 P2P |
| CR | P2P Completion Redirect | 对相应 P2P Completion 的路由进行上游重定向，使响应路径受控；不能当成普通读写权限检查 |
| UF | Upstream Forwarding | 约束适用组件的请求向上游转发，参与跨 Function/端口隔离；精确适用规则取决于组件类型 |
| EC | P2P Egress Control | 利用 egress control vector 等机制约束相应 P2P 请求可从哪些出口转发；不是任意可编程的软件防火墙 |
| DT | Direct Translated P2P | 对适用的已翻译 P2P Memory Request，允许直接路由到 peer 出口，不受 P2P Request Redirect 和 P2P Egress Control 机制约束；不改变 Completion 的处理，隔离分析必须把它纳入 |

这些功能在 Linux `include/uapi/linux/pci_regs.h` 中分别对应 `PCI_ACS_SV`、`PCI_ACS_TB`、`PCI_ACS_RR`、`PCI_ACS_CR`、`PCI_ACS_UF`、`PCI_ACS_EC`、`PCI_ACS_DT`。具体路由优先级和适用对象应查 ACS 规范；文末提供一份历史 ACS ECN 公开副本，帮助理解原始定义，它不替代目标版本的现行规范。[R11][R20]

### 10.3 Capability 与 Control

在 `lspci -vv` 中可能看到如下**示意输出，不是任何机器的实测**：

```text
ACSCap: SrcValid+ TransBlk+ ReqRedir+ CmpltRedir+ UpstreamFwd+ EgressCtrl- DirectTrans+
ACSCtl: SrcValid+ TransBlk- ReqRedir+ CmpltRedir+ UpstreamFwd+ EgressCtrl- DirectTrans-
```

- `ACSCap` 表示宣告支持的功能。
- `ACSCtl` 表示当前相关控制状态。
- `TransBlk-` 不能孤立解释成“系统不安全”：它可能是启用 ATS 所需的路径配置之一。
- `ReqRedir+` 也不能独立证明整机隔离成立：还要看路径中其他组件、DT、身份与平台行为。

实际输出字段名随 pciutils 版本变化。配置读取失败或权限不足时，工具展示也可能不完整。

### 10.4 ATS 与 ACS 的关系

ATS 需要设备向主机请求翻译并发送相应已翻译请求。如果适用路径开启 Translation Blocking，这些请求可能被阻止。因此 ATS 的使能与路径上的 ACS 控制需要统一管理。

同时，Direct Translated P2P 允许某些经过翻译的请求绕开普通的上游重定向路径。这说明：

```text
ATS：这个请求如何拿到授权翻译？
ACS：这个请求可走哪条路径，是否要阻断或重定向？
IOMMU/平台：这套身份、翻译和路径是否共同满足隔离要求？
```

ATS 不取代 ACS，ACS 也不取代 IOMMU。逐个功能“都显示支持”仍不能代替整条路径和生命周期的验证。

### 10.5 ACS 和 IOMMU Group

Linux IOMMU Group 表示当前内核与平台认定需要作为一个所有权/隔离单元管理的设备集合。它受真实拓扑、ACS、DMA alias、多功能设备隔离能力和内核 quirks 等因素影响。[R9]

典型例子：一个缺少足够隔离能力的交换机下有两个端点，内核可能把相关设备放入同一 group。不能仅因为它们有不同 BDF，就把它们安全地交给互不信任的虚拟机。

检查隔离需要沿端点到根的路径逐层看，不能只看端点上有没有 ACS Capability。反过来，没有某一项 Capability 也不自动证明必须同组，内核可能有组件类型规则或经过验证的硬件 quirk。

### 10.6 ACS override 为什么不能制造隔离

某些环境通过补丁或相关配置改变内核对 ACS/分组的判断。改变分组展示不会给缺失的交换机硬件增加重定向和源检查能力。

对于互不信任的设备/租户，应验证真实硬件隔离路径。把一个组拆成多个组，只能说明软件判断被改变，不能作为安全隔离的证据。

### 10.7 性能与隔离的权衡

本地 P2P 可以节省上行带宽和根部往返；上游重定向可能增加延迟或让原本可用的 P2P 不再可达。允许直接已翻译 P2P 又会增加信任和翻译一致性的分析要求。

所以 ACS 不能归纳成“全开一定最好”。正确做法是根据平台支持、信任边界、IOMMU/ATS 模式和 P2P 需求，由系统软件配置适用控制，并用实际拓扑验证。

## 11. SR-IOV、直通与 IOMMU Group

### 11.1 PF、VF 与 SR-IOV

**SR-IOV，Single Root I/O Virtualization**允许一个支持它的设备暴露多个可枚举的 Virtual Function。

- **PF，Physical Function**：具有相应管理能力的物理功能，通常负责 VF 创建、资源配置等。
- **VF，Virtual Function**：提供精简配置和设备功能，可分配给不同软件实体。
- VF 有自己的 PCI 身份与资源，但通常与 PF/其他 VF 共享设备内部硬件。

```text
一张网卡
  PF：配置 VF 数量、交换策略、资源等
  VF0 -> VM A
  VF1 -> VM B
  VF2 -> 主机或其他工作负载
```

SR-IOV 不是软件凭空模拟完整物理设备；VF 的数量、队列、隔离、迁移和复位能力都受设备实现限制。ARI 可能用于支持更多 Function 编号，但 ARI 与 SR-IOV 是不同特性。[R12]

### 11.2 VFIO 与直通

VFIO 使用户态能够在受控框架中使用设备，例如映射设备资源、配置 DMA 与中断。现代系统还可能通过 IOMMUFD 等接口管理 I/O 地址空间；具体使用方式取决于内核和 VMM 版本。

设备直通至少涉及：

1. 从宿主机原驱动切换所有权。
2. 按隔离单元建立 DMA 管理。
3. 将允许的设备资源与中断提供给用户态/VMM。
4. 配置设备 DMA 地址空间与虚拟机内存关系。
5. 在重启、退出和重新分配时可靠停止 DMA 并复位。

组内不一定要求每个 PCI Function 都以同样形式呈现给客体，但组的可用性和所有权必须满足 VFIO、IOMMU 后端及设备驱动约束，不能留下不受控的 DMA 来源。[R9]

### 11.3 二阶段翻译与嵌套虚拟化视角

概念上可能出现：

```text
Guest VA -- Guest 页表语义 --> Guest PA
Guest PA -- Host 管理的映射 --> Host PA
```

普通 VFIO 场景中，客体为设备编程的 DMA 地址如何映射到 Host PA，由 VMM、IOMMU 和客体是否使用 vIOMMU 共同决定。不能把 CPU 的两级页表路径直接等同于每次 DMA 的实际硬件路径。

vIOMMU、嵌套 IOMMU、客体 ATS、PASID 和 SVA 透传是进一步的能力组合，需要分别检查硬件、主机内核、VMM、客体内核和设备驱动支持。

### 11.4 虚拟化常见限制

- FLR 并不保证清除设备所有共享资源状态。
- PF 复位可能影响多个 VF，设备管理策略必须考虑故障域。
- SR-IOV 不自动解决 VF 间所有内部数据泄露和资源争抢问题。
- 设备实时迁移需要厂商/驱动/VMM 提供状态保存与恢复等支持；存在 VF 不代表可迁移。
- Group 分得开不等于设备固件与内部隔离实现没有问题。

## 12. 电源管理、复位、热插拔和错误恢复

### 12.1 设备电源状态与链路电源状态

| 类别 | 例子 | 管理的对象 |
|---|---|---|
| 设备 D-state | D0、D1/D2（可选）、D3hot、D3cold | Function/设备的电源状态 |
| 链路状态 | L0、L0s、L1、L1 子状态等 | 链路活动与节能 |
| 系统状态 | suspend/resume 等 | 整机电源生命周期 |

D3cold 通常涉及供电被撤去等平台动作，不能等同于向普通配置寄存器写入一个值。设备进入低功耗后，配置访问和恢复能力也有相应限制。

**ASPM**允许链路在空闲时进入适用的节能状态。L1.1/L1.2 等子状态进一步节能，但恢复延迟更大，依赖两端和平台配合。

**LTR，Latency Tolerance Reporting**让设备报告可容忍的服务延迟，供平台电源策略参考；**OBFF**帮助提示适合处理流量的时机；**PME**用于相应电源管理事件与唤醒。它们不能统称为“开启后性能一定更好”。

### 12.2 常见复位方式

| 复位 | 大致作用域 | 注意点 |
|---|---|---|
| FLR | 支持它的 Function | 仍需软件停止业务、等待并恢复状态；共享硬件影响要看设备 |
| Secondary Bus Reset / Hot Reset | 桥下游层级/链路相关范围 | 可能同时影响多块设备 |
| Fundamental Reset | 设备基础复位，例如供电或相应平台复位信号 | 作用更广，依赖硬件与平台 |
| 设备专用复位 | 厂商定义的控制路径 | 不能从 PCIe 通用能力完全推断 |

复位不是恢复软件状态的魔法。队列、DMA 映射、中断、ATS/PASID/PRI、BAR 配置和设备固件状态可能需要按各自生命周期重新建立。

### 12.3 热插拔与意外移除

支持热插拔通常需要端口/插槽控制、供电管理、资源预留、固件与 OS 配合。机械上可以拔出来，不等于它具备热插拔设计。

计划移除可以先停止 I/O、卸载驱动和关闭设备；Surprise Removal 则可能在 MMIO 或 DMA 进行中发生。驱动必须能处理设备失联、读取异常值、超时和资源清理，不能继续无限访问失效 BAR。

### 12.4 AER

**AER，Advanced Error Reporting**提供更细的 PCIe 错误状态、记录与报告机制。常见分类：

- Correctable：硬件/协议能够纠正，业务可能继续；持续大量出现仍可能提示信号质量问题。
- Uncorrectable Non-Fatal：不可纠正，但不必然让整条链路或整个层级失效。
- Uncorrectable Fatal：严重错误，可能需要更大范围恢复。

常见错误名包括 Receiver Error、Bad TLP、Bad DLLP、Replay Timer Timeout、Completion Timeout、Unsupported Request、Completer Abort、Malformed TLP、Poisoned TLP、ECRC Error 等。它们的来源和严重度不能只靠名字或一条日志猜定，要结合状态寄存器、错误头和拓扑。[R13]

Linux 是否负责 AER，还受固件是否把相应控制权交给 OS 等条件影响。没有 Linux AER 日志，不能直接证明链路没有错误，也不能直接证明设备缺少 AER 能力。[R13]

**UR**表示目标不支持相应请求；**CA**表示 Completer 中止；**CRS**是配置请求相关的重试状态语义。它们不能全部理解为“链路线缆坏了”。

### 12.5 DPC 与恢复

**DPC，Downstream Port Containment**在相应错误条件下隔离下游，限制错误扩散。隔离后设备可能暂时不可达，系统需要执行链路、设备与驱动恢复。

Linux PCI 错误恢复可能涉及驱动的 `error_detected`、`mmio_enabled`、`slot_reset`、`resume` 等回调；具体执行哪些回调与错误类型和驱动支持有关。[R14]

**Poisoned Data**通过标记表达数据可能不可信，并不保证硬件能自动恢复原始正确数据。AER、DPC、数据毒化和设备 ECC 应放在整体 RAS 中理解。

## 13. 其他重要特性地图

本章用于补齐知识范围。每项至少要知道它解决什么问题、依赖谁、不能保证什么。

| 特性/领域 | 用途、流程与条件 | 限制或观察入口 |
|---|---|---|
| P2P DMA | 一个设备直接访问另一个设备提供的 PCI 地址资源，减少主机内存中转 | 受拓扑、ACS、RC 路由和 Linux P2PDMA 支持范围约束；可见 BAR 不等于可以任意跨根 P2P |
| AtomicOps | FetchAdd、Swap、CAS 等原子事务，由支持的目标完成读改写 | Requester、路由路径、Completer 均需支持相应能力；不替代 CPU/设备整体内存模型 |
| TPH | TLP Processing Hints，携带处理位置等提示，尝试改善数据局部性 | 需要平台、设备和软件支持；提示不是强制调度承诺 |
| PTM | Precision Time Measurement，利用层级时间测量支持精确时间协作 | 需要相应 Requester/Responder 与路径支持，不自动等同于端到端应用时间同步 |
| LTR / OBFF | 描述可容忍延迟或提供流量处理提示，帮助系统节能 | 需电源管理策略配合；错误的延迟假设可能伤害实时业务 |
| L1 PM Substates | 在 L1 基础上进一步关闭部分链路相关电路 | 需两端、时钟/供电和平台支持；节能与唤醒延迟有权衡 |
| Resizable BAR | 从支持的尺寸中选择更大或更小 BAR 映射窗口 | 受地址空间、固件和驱动支持限制；不是扩大物理显存容量 |
| ARI | 扩展同一层级可表达的 Function 身份布局 | 需要上游支持转发；与 SR-IOV 配合但不提供 DMA 隔离 |
| Extended Tags / 10-bit Tags | 扩大可同时区分的事务标签范围，帮助维持更多在途请求 | 需相关端点和路径能力/软件使能，不能仅靠增大标签解决设备瓶颈 |
| VC / TC | 区分事务类别，配合独立虚拟通道资源与仲裁 | 端到端 QoS 需要整条路径支持，不保证现实系统启用多个 VC |
| ECRC | 为 TLP 提供可选端到端完整性检测 | 不等于加密或身份认证，也不修复业务数据 |
| VPD | Vital Product Data，用于产品标识等信息访问 | 内容布局和可写性取决于设备；它不是固件升级的通用接口 |
| VSEC / DVSEC | 厂商特定或指定厂商的扩展能力描述 | 必须结合相应规范/厂商定义解释，不能只凭 ID 猜语义 |
| DOE | Data Object Exchange，通过配置扩展能力交换标准化数据对象 | 是承载/发现机制，本身不提供完整安全策略或数据加密 |
| SPDM | 认证设备、测量与协商安全会话等安全协议，可在相应绑定下由 DOE 承载 | 需要信任根、证书/测量验证与策略；支持 DOE 不等于已经完成 SPDM 认证 |
| IDE | Integrity and Data Encryption，为适用 PCIe 流提供完整性与加密保护 | 需要密钥管理、端点/端口和平台支持；不能代替 IOMMU 的内存访问授权 |
| TDISP | TEE Device Interface Security Protocol，面向机密计算中的设备接口分配与状态管理 | 需要与 SPDM、IDE、设备及平台可信执行体系协同，普通 VFIO 直通不等于可信设备分配 |
| SR-IOV / MR-IOV | 前者围绕单根 I/O 虚拟化；后者面向多根共享场景的规范方向 | 不能由 SR-IOV 推导任意多主机共享；MR-IOV 的现实部署与产品支持需单独确认 |
| Non-Transparent Bridge | 在相互独立的主机地址域之间提供窗口、doorbell 等通信机制 | 不等于两台主机自动共享同一个一致性内存地址空间 |
| PCIe Fabric 管理 | 多交换机、多主机和动态资源组织中的拓扑与管理能力 | 具体产品模型差异很大，不能只靠单根桌面机模型解释所有部署 |

### 13.1 PCIe 与 CXL 的区别

CXL 使用 PCIe 物理层等基础设施，CXL.io 与 PCIe I/O 语义相关；CXL.cache、CXL.mem 则引入设备缓存与内存访问等额外协议能力。

因此，普通 PCIe DMA 不自动具备 CXL.cache 的缓存一致性协议，也不能把任意 PCIe 卡插入任意槽位就变成 CXL 内存设备。需要主机端口、设备、固件和 OS 共同支持相应 CXL 模式与功能。[R15]

### 13.2 安全机制分工

```text
身份/可信性：      设备认证、SPDM、平台信任策略
传输保护：        IDE 等链路/流保护
DMA 地址授权：    IOMMU、页表、权限
请求路径控制：    ACS 与拓扑
地址空间区分：    RID、PASID
翻译缓存一致性：  ATS 失效及 IOMMU 协作
设备分配生命周期：VFIO/IOMMUFD、复位、驱动及可选 TDISP
```

“数据在线上被加密”不能证明设备被授权访问了正确内存；“设备有独立 VF”也不能证明传输已加密。这些是不同保护维度。

## 14. Linux 观察与排障练习

### 14.1 练习前提

以下命令以只读观察为主，需安装 pciutils，部分信息需要管理员权限。输出受内核、固件、权限和虚拟化环境影响。WSL、容器或虚拟机里看到的拓扑未必是宿主机真实物理拓扑。

不要从“属性不存在”直接推出“硬件不支持”：可能是驱动、内核配置、权限或接口版本不同。也不要在不理解副作用时使用 `setpci`、写 `reset`、`remove`、`rescan` 或修改驱动绑定来做入门实验。

### 14.2 看设备和拓扑

```sh
lspci -Dnn
lspci -t
lspci -DPPnn
lspci -Dnnk
```

分别观察完整域号与 ID、树形拓扑、桥路径和驱动绑定。先选择一个真实存在的端点，记录它经过哪些桥到达 Root Port。

使用下面的交互输入，避免把教程中的示例 BDF 当作本机设备地址：

```sh
read -r -p '输入 lspci -D 中真实存在的 BDF: ' BDF
lspci -s "$BDF" -vv
```

重点寻找：

- `LnkCap`：端口/设备宣告的最大链路能力。
- `LnkSta`：当前协商的速度与宽度。
- `DevCap` / `DevCtl`：MPS 等能力与配置。
- `MSI` / `MSI-X`：能力、启用状态与表容量。
- `ATS`、`ACS`、`PRI`、`PASID`、`SR-IOV`：扩展能力与控制状态。

还要对路径中的上游桥逐个观察。端点性能和隔离可能被中间端口限制。

### 14.3 读 sysfs 资源与分组

下列命令沿用上一步输入的真实 `BDF`：

```sh
DEV="/sys/bus/pci/devices/$BDF"
cat "$DEV/vendor" "$DEV/device" "$DEV/class"
cat "$DEV/resource"
cat "$DEV/numa_node"
readlink -f "$DEV/driver"
readlink -f "$DEV/iommu_group"
```

`resource` 描述资源的范围和标志，不是让你直接读取设备内部数据。`numa_node` 为 `-1` 通常表示没有已知有效 NUMA 关联，不能解释成“设备不存在”。

如果设备存在 IOMMU Group 链接，可以读取其成员：

```sh
if [ -L "$DEV/iommu_group" ]; then
    ls -l "$DEV/iommu_group/devices/"
fi
```

系统存在 group 链接不代表已经完成安全直通配置；还需要确认 domain 类型、重映射模式、设备信任和中断隔离等。

### 14.4 观察 SR-IOV 与中断

```sh
if [ -r "$DEV/sriov_totalvfs" ]; then
    cat "$DEV/sriov_totalvfs" "$DEV/sriov_numvfs"
fi
if [ -d "$DEV/msi_irqs" ]; then
    ls "$DEV/msi_irqs"
fi
cat /proc/interrupts
```

这里只读取 VF 数量与 IRQ 信息。创建 VF、解绑驱动和直通会改变系统状态，应在单独实验环境中按设备文档执行。

### 14.5 看错误与 IOMMU 日志

```sh
dmesg --level=err,warn
journalctl -k -b
```

在日志中按设备 BDF 和关键词关联查看：`AER`、`DPC`、`IOMMU`、`DMAR`、`AMD-Vi`、`SMMU`、`fault`、`timeout`。其中 DMAR、AMD-Vi、SMMU 对应不同平台的相关实现，日志字段并不完全相同。

错误日志必须保留时间和上下文：先出现的 IOMMU fault 可能导致业务超时，后续驱动复位又可能引发链路事件。只截取最后一条报错容易把结果当原因。

### 14.6 按现象定位问题

| 问题 | 第一轮检查 | 第二轮方向 |
|---|---|---|
| 完全枚举不到 | 供电、插槽、固件、Host Bridge、链路状态 | 训练/均衡、硬件日志、资源描述 |
| 能枚举但驱动无法启动 | 驱动匹配、BAR、固件、DMA mask、中断分配 | 设备初始化、IOMMU fault、资源不足 |
| DMA 数据错误 | DMA API、方向、缓存同步、所有权和屏障 | 生命周期、越界、ATC/IOTLB 陈旧翻译 |
| 性能低 | LnkSta、上行共享、NUMA、队列和 CPU | MPS/MRRS、outstanding、翻译 miss、设备瓶颈 |
| 直通分组不符合预期 | 整条桥路径与 IOMMU Group 成员 | ACS、DMA alias、quirk、硬件隔离能力 |
| 开 ATS 后出错 | 实际启用者、IOMMU 支持、ACS TB、失效 | PASID/队列上下文、复位/解绑竞态 |
| 周期性 AER | 错误类型、BDF、负载/温度相关性 | 信号质量、供电、固件、设备协议行为 |

### 14.7 初学者最容易误判的地方

1. **看到 PCIe x16 就按 x16 带宽估算**：先核实实际 lane 和协商速度。
2. **把 DMA 地址当 CPU 指针**：地址解释者与地址空间不同。
3. **认为 CPU 不参与 DMA，就不需要驱动同步**：驱动负责映射、所有权和顺序。
4. **认为所有写都等得到 Completion**：Memory Write 常为 Posted，配置写则不同。
5. **认为 coherent 内存不需要任何屏障**：一致性与顺序是两个问题。
6. **把 ATS 当 IOMMU bypass 的性能按钮**：它引入翻译缓存与安全协作契约。
7. **把 PASID 当 PID**：它是设备请求使用的地址空间标识。
8. **认为 PRI 会自动把所有内存永久锁定**：它提供请求页处理的协议，成功与策略由系统决定。
9. **把 ACS Group 拆分当硬件隔离证明**：必须验证真实硬件路径。
10. **以为 P2P 不经过 CPU 就必然可用**：地址可达性、路由与内核支持仍有条件。
11. **以为 SR-IOV 的 VF 完全不共享硬件**：复位、带宽和内部资源可能共享。
12. **把所有 AER 都归因于接触不良**：不支持请求、超时和设备行为也会报错。

## 15. 学习路线、检查题与术语表

### 15.1 四周学习路线

| 阶段 | 学什么 | 可检查的产出 |
|---|---|---|
| 第 1 周：发现与拓扑 | 1、2、4 章；认识 RC/RP/EP、BDF、BAR | 画出一台机器从根端口到端点的树，标实际链路速度/宽度 |
| 第 2 周：访问与数据流 | 3、5、6 章；MMIO、DMA、中断与排序 | 分别画出设备收包与发包的数据/控制/通知路径，标注各类地址 |
| 第 3 周：隔离与虚拟化 | 7–11 章；ATS/ACS/PASID/PRI/SR-IOV | 用一页解释 ATC miss、PRI 缺页、映射撤销以及 P2P 隔离 |
| 第 4 周：可靠性与实战 | 12–14 章；错误恢复与能力观察 | 形成设备信息表、错误排查表和带宽瓶颈分析，不改写生产配置 |

如果目标是 Linux 驱动开发，下一阶段深入 PCI probe/remove、DMA API、MSI-X 和错误恢复；如果目标是虚拟化，重点深入 IOMMU、VFIO/IOMMUFD、复位与隔离；如果目标是硬件/验证，继续读 LTSSM、时序、电气、协议编码和合规测试。

### 15.2 自测：能解释这些问题才算建立模型

1. 为什么能 `lspci` 看到设备，但驱动仍可能完全无法 DMA？
2. CPU 写 BAR、设备读内存、设备发 MSI-X，分别是谁发起什么事务？
3. 为什么 Memory Read 与 Memory Write 的延迟和吞吐特征不同？
4. 为什么改了 IOMMU 页表后，仍不能立即复用物理页？
5. 同一个 GPU 同时服务两个进程，RID、PASID 分别负责区分什么？
6. ATS miss、Translation Completion 和 PRI Page Response 有什么不同？
7. 为什么两个不同 BDF 的设备可能必须位于同一个 IOMMU Group？
8. 为什么启用 P2P Request Redirect 后，设备间 P2P 可能无法工作？
9. 为什么 `ReqRedir+` 还不足以证明所有已翻译 P2P 都经过根？
10. 为什么设备 FLR 后，驱动不能直接沿用全部旧队列和翻译状态？

答案应能在前文找到。若某个问题只能背缩写，回到对应流程图，从发起者、地址、路由、权限、完成和回收六个角度重新解释。

### 15.3 术语速查

| 缩写 | 全称 | 一句话理解 |
|---|---|---|
| RC / RP / EP | Root Complex / Root Port / Endpoint | 主机根部、根端口和终端设备 |
| BDF | Bus / Device / Function | PCI Function 的层级地址 |
| RID | Requester ID | 请求发起者身份，通常来自 BDF 编码 |
| BAR | Base Address Register | 描述 Function 暴露的地址资源 |
| ECAM | Enhanced Configuration Access Mechanism | 标准化配置空间访问布局 |
| TLP / DLLP | Transaction / Data Link Layer Packet | 事务报文与链路管理报文 |
| LTSSM | Link Training and Status State Machine | 链路训练与状态控制 |
| MMIO | Memory-Mapped I/O | CPU 经地址映射访问设备 |
| DMA | Direct Memory Access | 设备发起数据访问 |
| IOVA | I/O Virtual Address | 设备 DMA 使用的虚拟地址 |
| IOMMU | I/O Memory Management Unit | 设备侧地址翻译与保护 |
| IOTLB | I/O Translation Lookaside Buffer | IOMMU 翻译缓存 |
| ATC | Address Translation Cache | 设备侧翻译缓存 |
| ATS | Address Translation Services | 设备申请并缓存地址翻译的协议 |
| PASID | Process Address Space ID | 请求携带的地址空间标识 |
| PRI | Page Request Interface | 设备请求主机处理页面可用性的接口 |
| SVA | Shared Virtual Addressing | 让设备使用进程关联的虚拟地址空间 |
| ACS | Access Control Services | 部分源检查、路径限制和 P2P 控制 |
| PF / VF | Physical / Virtual Function | SR-IOV 中的管理功能与虚拟功能 |
| VFIO | Virtual Function I/O | Linux 用户态设备访问与直通框架 |
| MPS / MRRS | Max Payload / Max Read Request Size | 数据载荷与读请求长度上限 |
| MSI / MSI-X | Message Signaled Interrupts | 通过写事务发送的中断机制 |
| AER / DPC | Advanced Error Reporting / Downstream Port Containment | 详细错误报告与下游错误隔离 |
| ASPM | Active State Power Management | 活动状态下的链路节能机制 |
| FLR | Function Level Reset | Function 级复位 |
| DOE | Data Object Exchange | 设备与主机交换数据对象的机制 |
| SPDM | Security Protocol and Data Model | 设备认证、测量与会话相关安全协议 |
| IDE | Integrity and Data Encryption | 互连数据完整性与加密保护 |

## 16. 参考资料与源码入口

以下资料用于进一步核对协议与实现。PCI-SIG 规范是协议语义的最终依据，Linux 文档描述 Linux 的接口与支持范围，源码用于理解某一版本的实现；三者不能互相替代。

- **[R1] PCI-SIG：PCI Express 技术与规范入口**：<https://pcisig.com/pci-express-technology>。用于定位各代际规范、公开介绍和配套规范。
- **[R2] PCI-SIG：PCI Express 6.0 Specification**：<https://pcisig.com/pci-express-6.0-specification>。关注 PAM4、FLIT、FEC 与带宽口径。
- **[R3] Linux PCI 文档索引**：<https://docs.kernel.org/PCI/index.html>；驱动入门：<https://docs.kernel.org/PCI/pci.html>。
- **[R4] Linux Dynamic DMA mapping Guide**：<https://docs.kernel.org/core-api/dma-api-howto.html>；详细接口：<https://docs.kernel.org/core-api/dma-api.html>。
- **[R5] Linux Bus-Independent Device Accesses**：<https://docs.kernel.org/driver-api/device-io.html>；内存屏障：<https://docs.kernel.org/core-api/wrappers/memory-barriers.html>。
- **[R6] Linux MSI Driver Guide HOWTO**：<https://docs.kernel.org/PCI/msi-howto.html>。
- **[R7] Linux Shared Virtual Addressing**：<https://docs.kernel.org/arch/x86/sva.html>。以 x86 为背景介绍 ATS、PRI、PASID 与 SVA，不能把架构细节直接推广到全部平台。
- **[R8] Linux v6.6 ATS/PRI/PASID 能力管理源码**：<https://github.com/torvalds/linux/blob/v6.6/drivers/pci/ats.c>。
- **[R9] Linux VFIO 文档**：<https://docs.kernel.org/driver-api/vfio.html>；IOMMUFD：<https://docs.kernel.org/userspace-api/iommufd.html>。
- **[R10] Linux PCI Peer-to-Peer DMA Support**：<https://docs.kernel.org/driver-api/pci/p2pdma.html>。
- **[R11] Linux v6.6 PCI 寄存器与 ACS 实现入口**：<https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/pci_regs.h>；<https://github.com/torvalds/linux/blob/v6.6/drivers/pci/pci.c>。后者可查 `pci_acs_enabled()`、`pci_acs_path_enabled()` 等路径判断。
- **[R12] Linux PCI Express I/O Virtualization HOWTO**：<https://docs.kernel.org/PCI/pci-iov-howto.html>。
- **[R13] Linux PCI Express AER Driver Guide HOWTO**：<https://docs.kernel.org/PCI/pcieaer-howto.html>。
- **[R14] Linux PCI Error Recovery**：<https://docs.kernel.org/PCI/pci-error-recovery.html>。
- **[R15] Linux CXL 文档索引**：<https://docs.kernel.org/driver-api/cxl/index.html>。
- **[R16] Linux PCI sysfs 接口**：<https://docs.kernel.org/PCI/sysfs-pci.html>；稳定 ABI 描述可在内核树 `Documentation/ABI/` 中按 sysfs 属性名查找。
- **[R17] DMTF SPDM 规范入口**：<https://www.dmtf.org/standards/spdm>。DOE 是 PCIe 承载机制，SPDM 的协议和信任模型需查 DMTF 相应规范。
- **[R18] PCI-SIG：PCIe 7.0 Webinar Q&A**：<https://pcisig.com/blog/pcie-70-specification-next-generation-performance-meet-needs-advanced-ai-applications-webinar>；Gen7 规范入口：<https://pcisig.com/PCIExpress/Spec/Base/_7.0>。用于核对 Gen7 速率、双向带宽以及 Gen6/7 FLIT 布局。
- **[R19] PCI-SIG：ATS 1.1 与 PASID Translation ECN 入口**：<https://pcisig.com/PCIExpress/Specs/Base/AddressTranslationServices_1.1>；<https://pcisig.com/PCIExpress/ECN/Base/PASIDTranslation>。下载权限可能受 PCI-SIG 访问政策限制。
- **[R20] PCI-SIG ACS ECN 历史公开副本（MIT 托管）**：<https://pdos.csail.mit.edu/~sbw/links/ECN_access_control_061011.pdf>。用于理解七类 ACS 控制及 Direct Translated P2P 的例外；现行细节以对应版本正式规范为准。
- **[R21] Linux PCI 电源管理与 Host Controller 文档**：<https://docs.kernel.org/power/pci.html>；<https://docs.kernel.org/PCI/controller/pci-controller-drivers.html>。
- **[R22] PCI-SIG 安全相关 ECN 入口**：DOE：<https://pcisig.com/PCIExpress/ECN/Base/DataObjectExchange_1.1>；CMA-SPDM：<https://pcisig.com/PCI%20Express/ECN/Base/ComponentMeasurementandAuthenticationRevised>；IDE：<https://pcisig.com/PCI%20Express/ECN/Base/IntegrityandDataEncryption>。用于进一步核对数据对象交换、认证和传输保护的职责分工。

进一步看 Linux v6.6 时，推荐按问题进入源码，而不是从头阅读整个目录：

| 想回答的问题 | 入口 |
|---|---|
| 配置如何扫描、Function 如何建立 | `drivers/pci/probe.c` |
| BAR 与桥窗口如何分配 | `drivers/pci/setup-res.c`、`drivers/pci/setup-bus.c` |
| ATS / PRI / PASID 如何探测与使能 | `drivers/pci/ats.c` |
| ACS 如何参与路径判断 | `drivers/pci/pci.c`、`drivers/pci/quirks.c` |
| SR-IOV VF 如何创建 | `drivers/pci/iov.c` |
| 中断向量如何管理 | `drivers/pci/msi/` |
| AER / DPC / ASPM / 热插拔如何处理 | `drivers/pci/pcie/`、`drivers/pci/hotplug/` |
| IOMMU domain / fault / SVA 如何管理 | `drivers/iommu/`，再按 Intel、AMD、Arm 选具体驱动 |
| VFIO 如何管理设备生命周期 | `drivers/vfio/` |

阅读实现时优先追踪“启用 → 正常访问 → 映射变化 → 失效 → 停止/解绑 → 复位”这一整条链，才能理解这些能力为什么需要同时考虑性能、正确性和隔离。
