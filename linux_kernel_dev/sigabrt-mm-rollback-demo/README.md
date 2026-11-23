# ARM64 SIGABRT 检查点 demo

## 当前状态

- 内核代码：`~/Git/OLK-6.6-2`，提交`de4fb9a1670f`。
- 当前交付仅为文档：`documents.tar.gz`及`documents.tar.gz.sha256`。
- 旧完整代码/二进制归档与重复备份已清理，历史操作以09号文档记录为准，不代表文件仍存在。
- 用户态测试源码只在本工作目录中有独立副本，因此保留在`tests/`，不放入文档包。

## 目录

```text
01～10-*.md              需求、设计、审核和历史记录
00-交付清单.md           当前交付范围
测试报告.md / 覆盖率.md    已执行结果与未覆盖边界
COMMIT.txt              Git提交信息
documents.tar.gz        仅文档压缩包
documents.tar.gz.sha256  文档包SHA256
feature.config          启用功能的配置片段
disabled.config         关闭功能的配置片段
.config / .config.disabled  实际验证配置
tests/                  测试源文件、脚本、原始日志、覆盖数据与报告
evidence/               构建/样式/补丁检查日志、已测产物哈希及清理明细
```

根目录是文档唯一入口，不再使用重复的`documents/`说明目录。
07～09号文档保留历史审核与归档过程；当前文件保留策略以本说明、00和10号文档为准。

## 文档归档

```sh
python3 tests/build.py pack
sha256sum --check documents.tar.gz.sha256
```

`pack`只处理现有Markdown和COMMIT.txt，不重新构建、运行测试或生成代码归档。

## 验证材料

完整验证日志为`tests/qemu_output.log`（其中记录100轮、1400/1400结果），关闭配置日志
为`tests/qemu_disabled.log`。原始gcda/gcno、coverage.info及HTML报告保留；只删除可以
据此重建的重复`.gcov`文本与辅助源码链接。原始产物哈希在`evidence/manifest-tested.json`，
属于历史验证记录，不要求已经清理的可再生产物仍然存在。

测试复现见`tests/README.md`。本次整理不改动内核提交，也不产生新的功能验证结论。
