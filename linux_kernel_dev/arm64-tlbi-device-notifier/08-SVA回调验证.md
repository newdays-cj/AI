# SVA 拆分回调：隔离运行验证

## 范围与实现

这是用户授权隔离软件原型的下一阶段，仅补齐实际 SVA invalidation callback 的软件运行证据。第一阶段 `测试报告.md`、`prototype.patch` 和 `qemu-prototype.log` 保留为历史交付。

新增实现位于 `drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c` 和私有 `arm-smmu-v3-sva-test.h`。只有 CONFIG_KUNIT 开启时，ASID TLB、range ASID TLB 和 domain ATC 三个既有 leaf 才包含对象限定重定向及测试命名空间导出；生产非测试路径不增加 fallback，不改变 SVA ops 选择或错误处理。

隔离守卫先检查 in_task、当前 KUnit test，再查唯一命名资源，核对 gate 与确切 smmu/domain 身份；不读取任意 test->priv，也不解引用来路对象。重定向位于命令构造/特性访问之前。KUNIT=n 守卫和导出消失。

独立模块 `smmu-sva-callback-test.ko` 由 cases/fixture 两个源文件构成，suite 为 `tlbi-smmu-callbacks`。每个参数用例有独立 fixture、真实 owned mm、普通内存 fake smmu/domain、固定事件区和安装位图。不会注册 fake SVA notifier，不调用 release/free 或真实设备析构。每次 typed activation 后必须通过版本绑定内部 hook 精确核验替身地址，全部三个目标确认后才准入 actual callback；void activation 返回本身不是成功证明。E_KUNIT_OOM 框架已知例外继续保留，不注入框架分配失败。

per-case exit 先关闭 gate，仅撤销已核实安装，再 mmput；命名资源及 backing allocations 交由正确 LIFO 顺序清理。没有 worker、异步 callback 或运行中修改 stub，部分安装时不进入 callback。假对象不匹配 guard 的落穿只做源码检查，负例只运行安全 predicate/runner。

## 实测环境与构建

沿用完整 QEMU ARM64 配置基线、gcc 13.3、Linux 6.6.0、QEMU 8.2.2 cortex-a72/virt/GICv3/4 CPU/2 GiB。共享源码生成文件未清理；仅同步新增改动到已有干净 `source-prototype/`，使用独立输出。

| 验证 | 实际结果 | 证据 |
|---|---|---|
| ARM64 Image/modules | 退出 0 | `callback-runtime-build.log`、`callback-runtime-build-status.txt` |
| 三个独立外部模块 | 编译、modpost、命名空间通过，退出 0 | 同上 |
| KUNIT=n main/SVA 对象 | 退出 0，无未定义 KUnit/static-stub 引用 | `callback-runtime-off.log` |
| 最终五文件累计补丁 | apply --check --cached 和 diff --check 通过 | `callback-runtime.patch`，未修改索引或创建 commit |
| 严格 checkpatch | 0 errors、1 FILE_PATH_CHANGES warning、0 checks，原始退出码 1 | `callback-runtime-checkpatch.log`，现有 ARM SMMU MAINTAINERS 目录已覆盖私有头，提示保留接受，不屏蔽 |

新增三个测试文件的严格文件检查为零 error/warning/check；脚本语法通过。工具的 Python 依赖沿用项目本地解包目录，无 SPDX traceback。

## 主会话手动 QEMU 证据

命令：`bash tests/run_qemu.sh > qemu-callback-runtime-host.log 2>&1`，退出 0。原始串口为 `qemu-callback-runtime.log`。

| 场景 | 观察结果 | 证据位置 |
|---|---|---|
| TLB-only/ATC-only/legacy/显式 split × 9 范围 × RANGE_INV 两值 × ATC mock 两种返回 | 每轮 144 个参数结果全通过；每个 row 独立 fixture | `callback_behavior` pass:144 fail:0 skip:0 |
| 精确命令入口参数和顺序 | ASID 或 range 单次；ATC 单次；legacy 与显式 split 为 TLB→ATC；range granule=PAGE_SIZE、leaf=false；full ATC 保留原 start 且 size=0 | actual callback 的事件断言 |
| gate 关闭、无资源、错误对象、两对象/零对象 | 每轮 6 个 predicate/runner 负例通过，零设备入口调用 | `callback_predicate_rejects` pass:6 |
| 安装 0/1/2/3 个 verified stub 的重复收尾 | 每轮 4 项通过，安装位清零，lookup 无替身，mm 已释放，不运行 fake callback | `callback_partial_cleanup` pass:4 |
| 第一阶段回归 | 每轮 notifier 11、纯 SMMU 2 项通过 | 既有两套件 Totals |
| 卸载与重载 | 三个模块各两轮成功加载、卸载；最终 PROTOTYPE_PASS、PROTOTYPE_EXIT=0 | 串口 LOAD/UNLOAD/PASS/EXIT 标记 |

每轮合计 11+2+154=167 个 KUnit 执行结果，两轮 **334 个结果通过**，无 fail/skip。只允许一次已有非法注册 WARN_ON_ONCE，日志中实际站点仍为 `mm/mmu_notifier.c:659 __mmu_notifier_register`；没有其他 WARN/OOPS。数量不是分支覆盖率，不将 suite 汇总额外计数。

## 仍不证明的内容

ATC 整个 helper 在 leaf 入口即被替换，本轮没有执行其中 ATS/master 判断、smp_mb、锁、4KB 编码、SID/PASID、batch 或 CMD_SYNC；TLB leaf 同样没有提交命令。mock -ETIMEDOUT 只证明 void callback 正常返回且保留既有忽略返回策略，不是实际超时处理。直接 ops callback 测试没有证明 bit0/MMIO 来源或 CPU TLBI→notifier 架构接入。

H1–H4/V/C、真实硬件、生产条件 SVA allocation、旁路、并发压力、完整 G1–G4 和 gcov 不在此阶段验收范围。

## 测试有效性与最终审查

| 实际源码变异 | 真实结果 | 证据 |
|---|---|---|
| ATC-only 中用 `if (size)` 漏掉 zero/full 范围调用 | 构建退出0、runner退出1；actual callback 的事件数为0/1，期望1/2 | `qemu-callback-mutant-atc.log`、`callback-mutant-atc-build.log` |
| legacy 合并顺序改成 ATC→TLB | 构建退出0、runner退出1；第一、第二事件 kind 与 TLB→ATC 预期不符 | `qemu-callback-mutant-order.log`、`callback-mutant-order-build.log` |
| 恢复、重建、再次运行 | SVA 源码 cmp/SHA256 与原始字节相同；Image/modules 和外部模块重建退出0；三套件两轮334结果通过 | `callback-mutation-summary.txt`、`callback-restored-status.txt`、`qemu-callback-restored.log` |

两个失败均到达预期 callback 断言，不是启动失败。变异只发生在任务快照，未修改共享源码或测试。初始串口、配置、Module.symvers、guarded leaves 和测试的保留检查通过，临时备份已清理。

最终正确性 gate `ses_e41b25305ffeAlr4s8v6fAmBtR` 返回 **APPROVE**，无阻塞正确性或隔离问题，核实了 pre-dereference 接点、当前测试/对象所有权、typed activation 精确核验、部分收尾、实际用例与变异/恢复日志。其唯一非阻塞跟进为把本报告的 pending 状态更新为实际完成，现已落实。Momus `ses_e41d92426ffemVZ7ZHyj5qng69` 的 [OKAY] 仅为计划可执行性。

本阶段已完成，软件 callback 运行证据通过；真实硬件和本报告列明的未测范围不变。第一阶段累计补丁保留为 `prototype.patch`；最新 **`callback-runtime.patch` 是从同一原始 HEAD 起算的五文件累计补丁，不应在 prototype.patch 上再次叠加应用**。测试源码/脚本位于 `tests/`，没有 git commit 或暂存。
