# LPC2026 技术摘要：VFIO Live Update, Road to Continuous DMA

## 报告元数据

- **演讲者**：Vipin Sharma (`vipinsh@google.com`)，Google
- **会议 / 微会场**：LPC 2026 **LiveUpdate Microconference**（注意：不是 VFIO/IOMMU/PCI Microconference）
- **时间 / 地点**：2026 年 10 月 6 日 17:50，Europe/Prague，Club H
- **证据时效说明**：本文基于演讲当日（2026-10-06）的幻灯片陈述与 v5 邮件系列状态，**不**代表对当前主线发行版的代码审计。状态字段按报告时点为准。

## 核心结论

1. **场景边界**：本文讨论的是**宿主机内核 kexec 式 Live Update (LU)**，即宿主机内核整体替换，**不是**虚拟机迁移，也**不是**内核 livepatch。(p2)
2. **目标（官方陈述）**：让 VFIO PCIe 设备在内核 LU 期间做到四点：DMA 持续不中断、不清 BME (Bus Master Enable)、不触发设备 reset、保留设备配置。(p2)
3. **当前实现不等于通用连续 DMA**：截至 v5（2026-07-14），设备**在 kexec 期间仍会 reset** 作为安全措施，LU 完成前 DMA 并不真正连续；目前只做到 VFIO FD 经 LU ioctl 保存与取回。(p3)
4. **依赖链未闭合**：材料称 PCI LU v9 于 2026-09-28 合入 **liveupdate 仓库**，不能据此推断完整 VFIO LU 已进入 Linux 主线；完整方案仍依赖未完成的 IOMMU LU。(p3)
5. **维护者反馈（offlist）**：要求提供端到端可演示方案，并警示多阶段推进存在被放弃或被取代的固有风险。(p4)

## 问题背景

VFIO 设备直通给用户态后，宿主机内核一旦更新通常要重启或 kexec，设备随之 reset，DMA 流被迫中断，已建立的 IOMMU 映射与设备配置丢失。对长时间运行的 DMA 工作负载（如 DSA 加速器、SRIOV 设备）而言，重启代价高，因此社区希望让 VFIO 设备跨宿主机内核替换仍保持 DMA 连续。

需要明确：这里的 "Live Update" 特指**宿主机内核 kexec**，整条链路（内存、IOMMU、设备、FD）都要在跨内核时保留并重建。它**不**等同于虚拟机热迁移，迁移发生在 VM 层且宿主机内核不变；也**不**等同于函数级 livepatch，livepatch 只替换个别函数，不触及设备 reset 语义。kexec 式 LU 整体替换内核，语义更接近宿主机 reboot，但要求设备层不感知中断，DMA 流跨内核连续。(p2)

## 核心技术与当前状态

### v5 范围与限制（官方陈述，p3）

- v5 于 2026-07-14 发出，建立在 PCI LU v7 之上；PCI LU v9 于 2026-09-28 合入 **liveupdate 仓库**。这是依赖项目的进展，不等于完整 VFIO LU 已合入主线。
- VFIO FD 通过 `liveupdate` ioctl 保存与取回。
- 对传入内核的保留设备，FD **只能**经 LU ioctl 取回，普通 cdev 文件 open 或 VFIO group ioctl 取 FD **会失败**，以避免绕过受控接管流程。
- **kexec 期间设备仍会 reset**，作为当前基础阶段的安全措施。
- 已加入 selftests；这些基础功能不能等同于通用持续 DMA。

### FD 保留的本质（分析）

v5 保留的是 "FD 可被新内核重新取回" 这条恢复路径，**不**保证 FD 编号相同，也**不**保证底层对象内存逐字节一致。它是**状态重建**而非 "同一个 FD 对象跨内核存活"。因此把 v5 描述成 "连续 DMA 已实现" 是不准确的：kexec 期间仍有 reset，DMA 在该窗口内并不连续。报告明确把 "设备在 kexec 期间 reset 作为安全措施" 列为 v5 的当前行为，并标注 "pending full LU"，说明真正的连续 DMA 要等完整 LU 落地后才成立。(p3)

### 维护者 offlist 反馈（官方陈述，p4）

- VFIO LU 特性将直接走官方 VFIO 树；
- 单加一个 `VFIO_PCI_LIVEUPDATE` (Experimental) config 不能给社区信心；
- 多阶段推进存在固有风险：努力可能被放弃或被取代；
- **需要展示完整端到端方案**，讲清整体目标。

## 路线选择

演讲给出两条上游化路线，**均未定案**。

### Option 1：VFIO LU for noiommu DMA (p5)

- 用 IOMMUFD 的 noiommu 特性承载 VFIO；
- 用 `memmap=xx!yy` 制造 emulated PMEM，转为 devdax 用于 DMA；
- 无需保留 iommu 硬件状态，解除 VFIO 对 IOMMU LU 的依赖；
- 作者希望以一个完整可用的受限功能路径推进上游，而不是仅依靠 experimental config 隔离未完成能力；
- 原型分支 `vfio/no_iommu_lu`，在 **Intel DSA** 上测试，LU 期间传输继续。

分析提示：noiommu 路线**没有 IOMMU DMA 隔离**，安全性弱于真实 IOMMU 直通；报告**未给出基准测试**数据，也**未声称适用所有硬件**，仅 Intel DSA 一例。Intel DSA 上的 "DMA continued" 是原型证据，不是通用保证。完整 VFIO LU 最终仍需 IOMMU LU（WIP，且反向依赖 VFIO LU），因此 noiommu 路线是阶段性起步而非终局。(p5)

### Option 2：Full LU implementation branch (p6)

- 建一个同时含 VFIO 与 IOMMU LU 代码的分支；
- 在两个子系统当前提案之上构建；
- 基于该 demo 分支提出下一版 series；
- 按 submitted 版本同步更新 demo 分支；
- 展示**不关闭宿主机 IOMMU** 的真实 LU 支持。

分析提示：这是 WIP 的联合 demo 分支，目标是让端到端可演示，但同样依赖未完成的 IOMMU LU。(p6)

## 未决问题

### 问题一：`pci_saved_state` 保留策略 (p7-9)

`pci_saved_state`（含 `pci_cap_saved_data`）记录 VFIO open 时保存的 PCI 状态，最后一个设备 FD 关闭时会尝试恢复。材料展示 `vfio_pci_core_enable()` 调用 `pci_save_state()` 和 `pci_store_saved_state()`，以及 disable 路径加载保存状态的代码。需要区分 **open 时的恢复快照** 与 **交接时设备的当前运行态**：二者用途不同，保存前者不能替代维持后者。(p7-8)

两条候选，**均未决策**：

- **Option A：在 KHO 中保存 state**。在 PCIe LU 里加逻辑把 `pci_saved_state` 写入 KHO。代价是额外维护负担：新内核若对 saved capability 或寄存器做任何变更，LU 就会断。已有提案让其布局更显式，但任何寄存器/capability 变更都会破坏 LU。(p9)
- **Option B：跳过保存**。不保存 `pci_saved_state`，沿用现有 VFIO 代码把 save/restore 视为 best-effort 的语义。(p9)

### 问题二：`can_finish()` / `finish()` 的回收语义 (p10)

LU 给子系统提供 `can_finish()` 与 `finish()` 回调汇报完成状态。问题场景：VFIO FD 已从 LUO 取回，但**未绑定到 IOMMUFD**（遗忘或 bind 出错）时，这两个 API 该如何处理？

- **Option 1：Block Finish**。阻止 LU finish，即使用户因错误无法 bind 到 IOMMUFD。幻灯片指出该选择可能使设备只能通过宿主重启恢复可用性。(p10)
- **Option 2：Reset Unopened Device**。自动复位未完成打开/绑定的设备，使系统无需重启即可回到可管理状态。(p10)

**分析**：前者侧重不提前结束接管流程，后者提供失败恢复出口；复位会打断设备工作，因此必须明确何时判定接管失败，以及与正在进行的绑定操作如何同步。

### 问题三：后续 series 顺序 (p11)

待 VFIO noiommu DMA 合入后，下一步走哪条**未定**：

- **Option 1**：先 VFIO noiommu SRIOV 保留，再 VFIO iommu DMA 支持；
- **Option 2**：先 VFIO iommu DMA LU，再 VFIO iommu 对 SRIOV 的 LU 支持。

## 后续发展启示（分析）

本节为分析，非官方陈述。

- **连续 DMA 的真实前提**：DMA 目标内存在所有权移交期间不可被错误回收或覆盖；使用 IOMMU 时，翻译路径须持续有效；设备不得被破坏性复位、清除 BME 或重新初始化。设备可以正常推进队列和更新状态，要求不是所有寄存器值冻结不变，而是新内核能正确接管仍在运行的设备。
- **"连续 DMA" 不等于 "vCPU / 应用无中断"**：宿主机 kexec 本身会让 CPU 与用户态进程经历中断与重启。这里承诺的是**设备层 DMA 流不中断**，不要外延到 vCPU 或应用层无中断。报告**未提出 IRQ 连续性** 的保证，本文不替其发明该结论。
- **内存与 IOMMU 保留是链式依赖**：noiommu 路线用 emulated PMEM 绕开 IOMMU 保留，是务实但局限的起步；一旦回到有 IOMMU 的真实直通，IOMMU LU 的滞后会把整条 VFIO LU 拖在 WIP 状态。
- **维护者反馈的关键信号**：报告转述的 offlist 反馈强调端到端演示，并担忧分阶段工作被放弃或替代。这提示后续补丁应说明完整依赖链、可重复的验证路径和失败恢复行为；不能将这份反馈扩大解释为社区已作出的统一政策或回退决定。
- **决策仍开放**：`pci_saved_state` 保留策略、`finish` 回收语义、后续 series 顺序三处均为开放问题，演讲只列选项，未给结论。

## 参考资料

- 官方议程页（含元数据与附件索引）：[LPC 2026 / Contribution 2616](https://lpc.events/event/20/contributions/2616/)
- 演讲 PDF（本文引用页码均对应此 12 页版本）：[LPC2026_vfio_liveupdate.pdf](https://lpc.events/event/20/contributions/2616/attachments/2182/4818/LPC2026_vfio_liveupdate.pdf)
- v5 邮件系列（2026-07-14）：[lore.kernel.org/kvm v5 series](https://lore.kernel.org/kvm/20260714151505.3466855-1-vipinsh@google.com/)

> 页码引用约定：本文中 `(pN)` 指上述 PDF 的第 N 页。
