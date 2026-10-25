# 开发与接口说明

## 资源与线程

BridgeService 管理配置、注册表、发现和 SDK；WebHost 管理窗口、请求路由和控制队列。UI 处理 COM/窗口及轻量查询，耗时工作进入 TaskQueue。退出拒绝新任务、清除未开始任务、等待在途任务，再停止推流/轮询/发现，释放全部设备、枚举和 SDK，最后释放 COM。连接或枚举中的 SDK 调用不强行中断，关闭可能等待它结束。

注册表修改/遍历受锁保护，工作线程使用快照；移除控制器先移出注册表，retired 集合保留到退出，使旧快照指针仍有效。每台相机独立操作锁串行连接、属性、录像和取帧；取帧使用 try_lock，在控制等待时让出。SDK 回调只更新状态/唤醒，不回调服务或持锁做耗时 SDK 工作。

后台独立重试计时、轮转、离线地址短探测；连接/枚举仍受全局 SDK 锁保护。多机行为必须补真机验收。主线程 COM STA 在 WebView2 创建前初始化，SDK 最后释放。

RPC 内部路由 ID 与窗口原始 ID 分离，只回复请求 HWND；窗口销毁清理路由和待消息。WebView2 异步初始化使用存活标记及窗口代次拦截过期回调。监看按 IP 合并最新帧，只推送独立窗口。前端按相机 IP / 请求序号拒绝旧回包。

## RPC

请求 `{id,method,params}`，回复 `{id,result}`；result 包含 ok、code、message，数组包在 cameras 字段内。

| 方法 | 参数 |
|---|---|
| status / logs / models.list | 状态、日志、SDK 型号 |
| cameras.discover | timeoutSec, modelId |
| cameras.connectByIp | ip, modelId, sshUser?, sshPass? |
| cameras.add | 上述字段加 name, cameraId?，复用连接 |
| cameras.disconnect/remove/rename/setId/setActive | ip 及对应字段 |
| props.list / props.set | ip；写入另带 code, value |
| record.start/stop | ip，状态回读确认 |
| monitor.open | 打开独立监看 |
| liveview.start/stop | 在线绑定相机监看 |
| liveview.quality/setQuality | ip；空 ip 为默认；写入带 imageQuality, compression |
| atem.connect | NOT_IMPLEMENTED |

相机状态含 phase、型号、lastError、lastErrorMessage、recordingKnown、transportEnabled。属性含 readable、writable、range、possible。写后未确认返回 READBACK_UNCONFIRMED。JSON 兼容实现以 double 储存数字，超过 2^53−1 的值按字符串返回，拒绝不精确写入；SDK 8/16/32/64 位数值按实际宽度和符号解析。128 位和字符串属性暂不提供数值编辑。

## 配置与边界

按 EXE 根目录加载，缺失时从 config.default.json / mapping_rules.default.json 初始化。旧配置迁移为 schemaVersion=1，分配缺失 ID，保留绑定/规则，旧全局画质成为默认；旧 USB 绑定保留但不自动连接。冲突或损坏配置显式失败。

同目录 .tmp 写入，检查 flush/close，复制 .bak，再 MoveFileEx 原子替换；配置提交失败同时恢复内存。单机 imageQuality=-1 / compression=-2 表示继承默认，单机设置不修改另一相机或默认值。

ATEM 没有真实事件来源，连接/发送未实现。映射必须显式指定目标，不能跟随 UI 选择；整条规则先校验，未知条件/动作和无效参数拒绝执行，Camera ID 路由留给下一阶段。

配置凭据、缓存、日志、历史 ZIP 不进入 Git；测试夹具只在隔离浏览器中注入，不为正式 UI 添加虚拟相机。
