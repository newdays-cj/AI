# ARM64 checkpoint demo

内核目录默认 `~/Git/OLK-6.6-2`，可用 `CKPT_KERNEL` 覆盖。工具链使用
`~/tools/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-*`，配置基线使用
`~/tools/qemu_arm64_defconfig`，initramfs 使用 Buildroot 的 `output/images/rootfs.cpio`。
宿主需要 Python 3、QEMU、lcov 和 genhtml；VM 内以 root 运行测试，宿主无需 root。

## 一键构建和测试

```sh
bash ~/workspace/sigabrt-mm-rollback-demo/tests/run_tests.sh
```

1400 次主场景压力回归加边界、外部控制和 KUnit：

```sh
CKPT_ROUNDS=100 CKPT_QEMU_TIMEOUT=600 \
  bash ~/workspace/sigabrt-mm-rollback-demo/tests/run_tests.sh
```

配置关闭的完整 Image 构建和原生行为测试，然后自动恢复启用配置并重新构建：

```sh
python3 ~/workspace/sigabrt-mm-rollback-demo/tests/build.py disabled-check
```

构建使用项目目录下独立的 `.config`/`.config.disabled`，不覆盖内核根目录原来的
`.config`；正常构建会更新内核树的生成头、对象文件和 Image。不要和其它内核构建并发运行。

## 文件

- `demo.c`：14 场景，可循环 1～100 次；每个场景独立子进程，父进程不回滚。
- `edges.c`：10 个权限、期限、资源准入及故障注入场景，恢复失败采用三个原线程。
- `external.c`：ptrace、cgroup v1/v2 迁移、冻结拒绝和解除后的正常行为。
- `disabled.c`：关闭配置时的 EINVAL、正常 pthread 和 SIGABRT 行为。
- 内核 `kernel/task_checkpoint_test.c`：独立 `.ko` 中的 KUnit 准入契约测试。
- `run_qemu.py`：静态编译程序，生成叠加 initramfs，启动 VM、收集日志及 gcov。
- `init.sh`：挂载 proc/sysfs/debugfs/cgroup，加载 KUnit、运行测试、9p 导出覆盖数据。

单轮和压力运行统一使用`qemu_output.log`，日志内记录轮数和汇总；关闭配置日志
为`qemu_disabled.log`。不再为压力运行复制一份相同日志。覆盖率为 `coverage.info`、`coverage_summary.txt` 和
`gcov/coverage_html/index.html`，原始 gcda/gcno 与源码链接一起保存在 `gcov/`。

`build.py pack`只生成文档包，不再打包源码、补丁或二进制，不重新构建或运行测试。
内核代码位于`~/Git/OLK-6.6-2`，提交`de4fb9a1670f`；本目录的用户态测试源与脚本
仍保留用于复现。可执行文件和initramfs会在下次运行时重建。

原始gcda/gcno及HTML报告保留；重复.gcov文本和辅助源码链接可以由run_qemu.py重新生成。
整理前的验证产物哈希记录位于`../evidence/manifest-tested.json`，仅作历史证据。

在归档目录校验交付文件：

```sh
sha256sum --check documents.tar.gz.sha256
```

## 实验接口

```c
epoch = prctl(PR_TASK_CHECKPOINT_DEMO, PR_CKPT_PREPARE, thread_count, 0UL, 0UL);
/* 向全部原线程发布 epoch，各线程先退出应用锁/barrier，再分别调用： */
ret = prctl(PR_TASK_CHECKPOINT_DEMO, PR_CKPT_ARRIVE, epoch, 0UL, 0UL);
if (ret == 0) {
        /* 初始运行，可以创建/退出/join/detach 线程。 */
} else if (ret == 1) {
        /* 默认 SIGABRT 后的恢复分支，同 TGID、原 TID。 */
}
```

程序可使用内核 UAPI 头，或参考测试里的实验常量。记录到检查点的线程真正内核销毁
被延迟，但 join/detach 和栈复用必须能进行。恢复移除新增线程，消费检查点；第二次
默认 SIGABRT 按原生语义终止。放弃快照使用 DROP。

## 测试配置专用控制点

只有 `CONFIG_TASK_CHECKPOINT_DEMO_KUNIT_TEST` 启用时创建
`/sys/kernel/debug/task_checkpoint_demo/`：

- `exit_pause` / `exit_waiters`：确定性暂停 raw exit，在共享 ABRT 入队后再放行。
- `fail_stage`：一次性失败注入，1=QUIESCENT 前，2=本线程切 mm 后，3=全部切换后提交前，
  4=快照分配前。阶段 1～3 必须以 SIGKILL 失败关闭，阶段4返回 ENOMEM。
- `live_groups` / `held_tasks`：测试收尾观测，等待 RCU/释放工作完成后必须都为0。

这些控制点不属于生产 ABI，正常功能配置不含它们。

## 已知边界

当前验证环境为 QEMU 8.2.2、cortex-a72、4 CPU、2 GiB、glibc 2.43、GCC 14.3。
本机 QEMU 的 max 模型出现启动前断言，因此默认使用不提供 SVE/SME 的 cortex-a72。
PAC、SVE/SME、MTE 的硬件路径没有在该 CPU 上得到运行验证；代码拒绝不支持的活动状态。

只支持受控程序和白名单系统调用；已有 AIO/io_uring、共享/设备等不兼容状态被拒绝。
不回滚文件、网络、数据库、时钟等外部副作用。保留任务仍可能出现在 procfs/原生任务计数中。
覆盖率不是100%分支覆盖，尚未实现的设计用例和未命中方向在项目覆盖报告中明确列出。
