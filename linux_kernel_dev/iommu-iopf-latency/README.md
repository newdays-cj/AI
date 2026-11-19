# IOMMU I/O page fault response latency

最新提交：`33eea22f74accb57c3c91ec62d3b58b160206798`。  
标题：`iommu: Trace I/O page fault response latency`  
基线：`aba0b453d75e1c80f97cb5faf5718925b6844997`

为通用 IOPF 框架新增 `iommu:iopf_latency`，记录从report入口到驱动page_response返回的纳秒耗时。无需新增CONFIG，不记录命令提交返回值；PASID有效性打印为 `pasid(v)=7` 或 `pasid(i)=0`，原始trace数据仍保留pasid_valid字段。不导出iopf_latency的tracepoint符号。

## 目录

```text
iommu-iopf-latency/
├── 00-交付清单.md        最终提交、补丁及验证范围
├── 需求说明.md
├── 测试报告.md           当前状态汇总
├── 覆盖率.md
├── patches/              当前两个本地提交的原始format-patch
├── tests/                适配最终打印格式的测试源码和运行脚本
└── SHA256SUMS            交付文件校验清单
```

## 应用补丁

```sh
git am /home/chenjun/workspace/iommu-iopf-latency/patches/*.patch
```

按顺序应用两个补丁：`a1cf4faa32fc` 新增通用IOPF耗时事件，`33eea22f74ac` 移除不需要的tracepoint符号导出。两份补丁均直接从对应本地提交导出；本次整理没有新增或改写提交。

## 验证状态

- 通用IOPF实现：ARM64 Image/modules、无EVENT_TRACING目标对象编译、kABI及QEMU软件链路验证通过；一次QEMU启动内三轮suite，共12次成功执行，21次预期模式跳过。
- 最后的 `pasid(v/i)` 文本格式调整及tracepoint导出删除尚未重新编译或运行测试。`tests/` 中解析脚本已适配，未冒用旧日志作为当前修订验证结果。
- 本包仅保留结果摘要及可复测源码，不包含历史资料、原始验证数据、镜像或归档工具目录。

## 后续复测

在包含最终补丁的内核根目录，先保存当前.config：

```sh
cp /home/chenjun/workspace/iommu-iopf-latency/tests/kernel.config .config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j20 Image modules
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=/home/chenjun/workspace/iommu-iopf-latency/tests modules
python3 /home/chenjun/workspace/iommu-iopf-latency/tests/run_qemu.py
```

这些测试使用RAM队列和寄存器，适用于专用QEMU；不证明物理设备事务恢复。运行结果写入tests目录，编译器、QEMU和Buildroot initramfs由运行环境提供。

## 归档约定及校验

目录统一为 `~/workspace/iommu-iopf-latency/`。按最新约定，不归档history、validation、tools或metadata目录，仅保留当前补丁、必要文档、测试源码及配置。

归档完整性检查：

```sh
sha256sum -c SHA256SUMS
```
