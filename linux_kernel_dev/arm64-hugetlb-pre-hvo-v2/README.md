# arm64 HugeTLB pre-HVO v2 归档

- `arm64-pre-hvo-v2/`：8 个可应用的补丁、说明信、补丁顺序、重放说明和 SHA256 校验文件。
- `hugetlb-pre-hvo/`：从 `tools/testing/selftests/mm/hugetlb-pre-hvo/` 复制的 QEMU 自测脚本、客户机测试源码和配置片段。
- `pre-hvo-validation/`：构建产物、QEMU 日志、审查报告以及初版和 v2 的测试记录。建议先阅读 `REPORT-v2.md` 和 `v2-QA.md`。
- `社区补丁方案对照.md`：对照 section-based vmemmap 优化与 arm64 HVO 两组社区补丁的目标、组成和实现逻辑，并附原始邮件链接。

验证目录保留了早期测试记录，其中部分镜像内存测试日志已被
`v2-mirror-policy.valid-*` 测试取代。判断镜像内存分配策略时，请以
后者为准。归档采用复制方式，原仓库文件和本地提交未改动。

校验补丁集时，在 `arm64-pre-hvo-v2/` 目录下运行
`sha256sum -c SHA256SUMS`。该目录的 README 中，补丁重放命令默认从
原内核仓库执行；如果从其他检出目录重放，请将 `PATCH_DIR` 改为本归档
中的补丁目录。
