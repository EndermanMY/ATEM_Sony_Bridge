# ATEM Sony Bridge

> GitHub 源码快照（2026-10-25）：此仓库保存 0.2.1-dev 开发源码，最新改动仍需最终复验。当前相机配置、凭据、日志、历史归档、完整运行目录和 SDK 二进制均未上传。Sony SDK 文件需从本机已获授权的 SDK 安装包补齐至 `thirdparty/sony_sdk`，WebView2 x64 Loader 库需补齐至 `thirdparty/webview2/x64`，之后再构建。下文涉及 archives/vendor/releases 的路径描述原本地工作区，GitHub 仓库不包含这些目录。


当前开发目标版本为 **0.2.1-dev**，面向 Windows x64 的便携开发版。现有便携目录尚未同步最新源码，最终复验和打包仍待完成。采用 Sony Camera Remote SDK 2.02、C++17、WebView2、离线 Vue 3 和 Element Plus，提供网络相机库、属性读写、录像控制与独立监看。

ATEM 控制尚未实现，入口已禁用；USB 向导暂未开放。单机真机与 30 分钟稳定性验收仍待设备上线，双机验收待第二台设备到位。

进展、待办与执行顺序见 [项目进展与后续计划](PROJECT_STATUS.md)。

## 目录

```text
工作区/
  AtemSonyBridge/             源码、默认模板、测试及本地 Git
  archives/                   原始源码/完整运行备份、历史 ZIP、旧笔记、迁移清单
  vendor/Camera Remote SDK Ver. 2.02.00/   官方原始资料
  releases/
    0.2.0-dev-original/        未改动的原可运行目录
    0.2.1-dev/                 便携开发版
    current.txt               run.bat 使用的运行版本
```

源码 `thirdparty/sony_sdk` 是编译和运行所需子集；vendor 是完整原始资料，两者均保留。第三方授权以随附 SDK 和依赖资料为准。本项目未引入 spdlog，JSON 使用原有兼容实现。

## 构建与启动

需要安装含 MSVC x64、Windows SDK 和 CMake 的 Visual Studio Build Tools，以及 WebView2 Runtime。双击 build.bat，或执行：

```powershell
.\tools\build.ps1
.\run.bat
# 从干净目录构建
.\tools\build.ps1 -BuildDirectory build-clean
```

脚本依次从 PATH、vswhere 和已安装 VS 目录发现 CMake，构建失败返回失败状态。版本由 CMake project VERSION 和 BRIDGE_PRERELEASE 生成；C++ 常量、文件资源、窗口标题、状态接口、UI 和便携目录共用该来源。

**更新前请退出程序。** 构建输出和便携运行目录分离，重复构建只更新程序资产及默认模板，不覆盖 config/config.json、config/mapping_rules.json、日志、WebView2 用户数据。首次安装迁移原运行目录配置，首次启动从默认模板初始化缺失文件。保留 EXE、三个核心/monitor DLL、CrAdapter/、ui/、config/ 的完整结构。

程序按 EXE 所在目录加载配置，应用日志位于 logs/bridge.log，不依赖启动工作目录。SDK 自身日志位置遵循 SDK 行为。

## 使用

- 相机与电脑接入同一网络，开启遥控拍摄。五步向导只有连接确认后才能进入成功步骤，入库复用已建立连接。访问验证开启时提供相机凭据。
- 左侧选相机，常用控制显示曝光/白平衡；高级属性支持分组、名称或十六进制属性码搜索。不可写属性禁用修改。
- 属性写入回读不匹配时显示 READBACK_UNCONFIRMED；操作型属性可能没有稳定回读值，需观察相机实际状态。超出 JavaScript 精确整数范围的值按文本展示并禁止数值写入。
- REC 根据相机实际回读；没有确认时显示状态未知。短片测试不删除相机素材。
- 独立监看支持 2×2 / 4×4、容量翻页、离线占位及停止/首帧等待/画面中断提示；停止或断线后 FPS 清零。
- 默认画质与单机画质分开保存，单机按 IP 绑定。修改默认值不隐式修改选中相机。
- 手动断开会暂停该机本次运行的自动重连；点击重新连接或重启恢复。退出时等待在途 SDK 操作结束。

## 测试与回滚

```powershell
ctest --test-dir build-clean -C Release --output-on-failure
.\tests\update.test.ps1 -BuildDirectory build-clean
node .\tests\ui.test.cjs
python .\tools\verify_backup.py ..\archives\baseline-20261018-155614
```

前端测试需要本机 Playwright 和 Edge/Chromium，可通过 PLAYWRIGHT_MODULE、BROWSER_EXE 指定绝对路径。这些不是程序运行依赖。夹具仅注入隔离测试浏览器，不进入正式界面。

详见 [验收记录](docs/ACCEPTANCE.md)、[文件迁移及回滚](docs/MIGRATION.md)、[开发说明](docs/ARCHITECTURE.md)、[后续路线](docs/ROADMAP.md)。
