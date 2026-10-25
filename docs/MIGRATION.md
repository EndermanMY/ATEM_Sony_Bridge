# 文件迁移与回滚

源码主目录不变。逐文件 SHA-256 清单位于 ../../archives/migration-20261018.json，共 571 条迁移、复制和缓存归档记录。

| 原位置 | 现位置 / 处理 |
|---|---|
| 根目录六份历史 ZIP、两份说明 | archives/history/，原文件名及版本保留 |
| 原始 Camera Remote SDK Ver. 2.02.00 | vendor/ 同名目录，逐文件校验 |
| build/bin/Release | releases/0.2.0-dev-original/，与最初完整备份逐项一致 |
| build、build-review 剩余生成缓存 | 校验归档到 archives/build-caches-20261018.zip 后清理 |
| _new23.txt、旧 README/CHANGELOG/CLAUDE | archives/legacy-notes/ |
| 未被引用的 spdlog 占位 | archives/legacy-code/spdlog/ |
| 原源码运行配置 | archives/legacy-config/config.json |
| 一次性重构脚本 | archives/maintenance-tools/ |
| 空 include | 移除；docs 已改为当前文档目录 |

路径均相对工作区。编译使用 thirdparty/sony_sdk 子集，完整官方资料放 vendor，不再依赖迁移前绝对路径。

## 回滚基线

archives/baseline-20261018-155614 包含 source.zip（原源码和依赖）、runtime.zip（原 EXE、全部 SDK/适配器、配置、规则、离线页面、日志及 WebView2 数据）和 manifest.json。共 414 个文件，ZIP 已逐项读取并校验 SHA-256。

先退出桥接程序，解压到一个全新目录：

```powershell
python .\tools\verify_backup.py ..\archives\baseline-20261018-155614 --extract-runtime ..\releases\restored-original
```

在该目录运行 AtemSonyBridge.exe 可恢复原程序和原配置。保留当前开发目录，不混用新旧 DLL；不要同时启动两版连接同一相机。源码可从 source.zip 解压到新目录，或使用本地 Git 的整理前基线。没有建立远程仓库。

原运行目录另保留在 releases/0.2.0-dev-original。恢复文件完整性与隔离解压可自动验证，恢复后的真机连接仍需设备在线确认。

## 旧笔记的有效信息

继续采用官方 CrAdapter 布局；访问验证遵循 SDK 指纹/凭据流程；JPEG 可能带 SDK 块头和尾部补齐，必须按 SOI/EOI 检查边界；设备和线程必须先于 SDK 释放。旧笔记中的双机稳定帧率、配对持久性和网络限制结论均待复验。
