# ARCS-MINI Lua 小应用

## 功能

小应用是由云端下发、在设备本地执行的 Lua 脚本。设备可根据产品配置提供：

- 全屏绘制矩形和文字。
- 接收功能键单击事件。
- 控制状态灯亮灭及闪烁。
- 通过扬声器播放指定频率和时长的蜂鸣声，使用系统音量。
- API 4 可按设备能力发起 HTTP 请求、处理 JSON 和请求文本播报。
- API 5 可通过系统音乐播放器播放 HTTP(S) 音频文件和直播（MP3、HLS/TS AAC）。
- API 5 支持查询前台小应用状态、声明操作并接收经过校验的语音操作。
- API 5 在设备提供 `synth` 能力时支持四声部实时音效合成。

从桌面进入小应用时停止当前语音输入和音乐播放，保留触发本次安装的交互响应 TTS；语音安装与远程推送安装均按此规则处理。
进入小应用或退出回桌面时结束当前交互，TTS 播完后不自动续聊，需重新唤醒，使新会话的 `start` 帧携带最新模式。
设备原有的单工、半双工或全双工模式不变，再次唤醒后的交互遵循原模式。运行中的小应用被另一个小应用替换时也结束当前交互，使下次 `start` 帧携带新应用身份。
运行期间的语音交互范围为重新生成、修改、切换、询问玩法及执行应用声明的操作；闹钟到点正常响铃提醒。
端侧限制无关工具和普通歌单播放；小应用可使用 `audio` 接口播放自己的在线音频。云端依据下文的 `start` 帧模式，在产生回复之前过滤无关意图。
功能键每次短按释放立即向小应用发送一次点击，脚本可根据点击间隔识别双击等操作；系统的多击快捷操作屏蔽，长按 3 秒返回桌面。
长按退出后继续按住不会触发关机；需先松开，再次长按才会按桌面规则关机。

运行期间保持正常电源状态，不进入空闲降亮度或休眠。自动关机仍沿用设备原有条件：
无用户活动累计 15 分钟、未接外部电源且电量不高于 30% 时，播放提醒后进入 10 秒关机倒计时。
按键和语音交互按原规则重新计时；脚本动画和蜂鸣声不视为用户活动。

## 内存与生命周期

小应用仅在 PSRAM 中运行，源码不写入 Flash 或 KV。
退出或重启后释放本地应用，再次使用需由云端重新下发。
API 3 起提供的 `storage` 可单独持久化少量应用数据，不保存源码。

替换应用时，候选版本和当前版本暂时并存。候选版本完成校验、启动回调和界面初始化后，
才替换当前版本；失败时保留仍驻留内存的旧应用。因此安装新应用时需留出两个 Lua 运行实例的内存。
相同版本复用仅适用于仍驻留内存的应用，不表示设备具有持久化应用库。

## MCP 接口

以下接口均使用标准 MCP `tools/list` 和 `tools/call` 机制，结果为
`{ "content": [{ "type": "text", "text": "..." }], "isError": false }`。
失败时 `isError` 为 `true`，`text` 提供简短错误说明。

### 查询能力：`ls.built_in.get_device_capabilities`

参数为空对象。成功结果的 `text` 是 UTF-8 JSON，包含：

- `schema_version`：能力结构版本，目前为 `2`。
- `firmware_info`：固件类型及构建版本。
- `hardware`：实际支持的屏幕、按键、音频、灯光及联网能力；`network` 为布尔值，表示具备联网能力，不表示当前在线。
- `runtime.lua_sdk`：启用小应用时提供 API 版本、源码字节上限和每个 Lua 实例的堆上限，以及 `storage` 的容量、有效期和写入间隔。API 4 还提供 `http`、`tts` 对象。

`runtime.lua_sdk.http` 包含 `methods`（字符串数组）、`max_pending`、`max_url_bytes`、
`max_headers`、`max_headers_bytes`、`max_body_bytes`、`max_response_bytes`、
`default_response_bytes`、`max_timeout_ms` 和 `default_timeout_ms`。
请求头字节数按每行 `名称: 值\r\n` 累计；超时单位为毫秒。
`runtime.lua_sdk.tts` 包含 `text_max_bytes`（UTF-8 字节）、`max_pending` 和 `timeout_ms`。
API 5 的 `runtime.lua_sdk.audio` 包含 `max_url_bytes`（511）、`max_players`（1）、
`live`（true）和 `state_query`（true）。使用在线音频需同时具有联网和扬声器能力。
API 5 的 `runtime.lua_sdk.synth` 包含 `sample_rate`（16 kHz）、
`max_voices`（4）、`max_notes`（256）、`max_duration_ms`（60,000）、`queue_depth`（4）、
`waveforms`、`loop` 和 `pause_resume`。使用合成器需同时具有扬声器能力；能力字段
不存在时，应用不得调用 `synth`。

使用 HTTP 需要 `hardware.network == true` 且 `runtime.lua_sdk.http` 存在；
使用 TTS 还需要扬声器及 `runtime.lua_sdk.tts`。接口或硬件能力缺失时不推断可用。
网络瞬时故障通过异步请求结果报告。

能力字段由实际产品配置决定。硬件存在不等于 Lua 可以访问该硬件；Lua 可用接口由
运行时 API 版本及固件配置确定。能力查询本身不表示设备支持小应用安装。

### 安装并启动：`ls.built_in.miniapp_install`

设备注册此工具即表示支持小应用安装。以下参数全部必填：

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 小应用标识 |
| `version` | string | 不可变版本标识 |
| `name` | string | 小应用名称；非空，最多 127 UTF-8 字节，保存供状态查询使用 |
| `url` | string | 裸 Lua 文件的签名下载地址，有效期为 300 秒 |
| `size` | integer | 文件准确字节数，至少为 1，不超过设备公布的源码上限 |
| `hash` | string | 文件 MD5，32 位小写十六进制字符串 |

设备比较当前驻留应用的 `id`、`version`、`hash` 及文件大小。匹配时复用当前版本；
否则下载裸 Lua，验证长度及 MD5，并在 PSRAM 中启动候选版本。
最终结果在校验与启动完成后返回，使用原 MCP call ID 关联请求。

安装失败不替换仍可用的当前应用。调用超时或结果未知时，不自动重放整个安装请求。
不提供额外的安装状态或进度查询工具。

### 查询前台小应用：`ls.built_in.miniapp_describe`（API 5）

无参数，`arguments` 可省略或为 `{}`；其他类型及多余字段报错。成功结果的 `text` 是 JSON：

```json
{
  "running": true,
  "app": {"id": "timer", "version": "v1", "name": "番茄时钟"},
  "state": "倒计时已暂停，还剩 1200 秒",
  "actions": {
    "pause": {"description": "暂停倒计时", "parameters": {}},
    "set_duration": {
      "description": "设置专注时长，并重置倒计时",
      "parameters": {
        "minutes": {"type": "integer", "description": "专注分钟数", "required": true, "minimum": 1, "maximum": 180}
      }
    }
  }
}
```

没有前台小应用或闹钟抢占时，正常返回 `{"running":false}`，`isError=false`。
`app` 保存本次成功安装的身份和名称；相同版本复用时更新名称。本地 ADB 安装以文件名（不含 `.lua`）作为名称。

`state` 每次查询时调用 `get_state()`，必须返回字符串或 `nil` / `json.null`。
缺失回调、类型不符、含 NUL 或超过 2,048 UTF-8 字节时返回 `null`。
`actions` 来自启动时唯一一次 `get_actions()` 调用，运行期间固定不变。
缺失 `get_actions` / `on_action` 中任意一个函数，或声明不合法时，返回 `{}`，拒绝所有操作。
声明中任意一项不合法都会使整份声明失效，不发布部分操作。

### 执行操作：`ls.built_in.miniapp_action`（API 5）

先查询上述工具，再按 `actions` 提供的声明调用：

```json
{"name":"ls.built_in.miniapp_action","arguments":{"action":"set_duration","arguments":{"minutes":40}}}
```

`action` 和 `arguments` 均必填，分别为非空字符串和对象；无参数的操作也传 `{}`。
运行器严格区分大小写，拒绝未知操作、未知或重复参数、缺失必填项、错误类型、非有限数字、越界及不在枚举中的值；
不会把字符串 `"40"` 转换成整数。校验通过后，才在 Lua 任务中串行调用 `on_action(name, args)`。
返回值必须为 `{ok = boolean, message = string}`，其中 `message` 最多 2,048 UTF-8 字节且不含 NUL。
MCP 返回 `isError = not ok`，`content[0].text = message`。回调返回类型错误、应用已退出、
应用已被替换或闹钟抢占均返回 `isError=true`；被拒绝的操作不留待恢复后重放。
Lua 异常或超预算遵循运行器既有规则，终止实例并返回错误；启动阶段异常仍保留旧实例。
操作若已执行但回包失败，不自动重试，云端应重新查询状态。

小应用工具使用统一的 `ls.built_in` 前缀：`ls.built_in.miniapp_describe`、
`ls.built_in.miniapp_action`、`ls.built_in.miniapp_install` 和 `ls.built_in.miniapp_exit`。

#### 操作声明约定

`get_state()`、`get_actions()` 应当无副作用，脚本不应在查询回调中修改应用状态。
启动成功后固定使用当时的操作声明及 `on_action` 函数；后续修改表或重定义函数不会更改已发布的协议。
以下是当前固件和云侧模拟器需要共同遵循的声明格式：

| 字段 | 约束 |
| --- | --- |
| 操作表 | 字符串键对象，最多 16 个操作，操作名 1–63 字节 |
| 操作定义 | 仅包含必填 `description`（1–512 字节）和 `parameters` 对象 |
| 参数表 | 最多 16 个参数，参数名 1–63 字节；无参数用空表 `{}` |
| 参数 `type` | 必填；`string`、`boolean`、`integer` 或 `number` |
| 参数 `description` | 可选字符串，最多 512 字节 |
| 参数 `required` | 可选布尔值，默认 false；与标准 JSON Schema 的 required 数组不同 |
| `minimum` / `maximum` | 可选有限数字，仅对数值参数有效，含边界，minimum 不得大于 maximum |
| `enum` | 可选非空数组，最多 32 项，每项必须匹配参数 type |

未知声明字段或约束会被拒绝，不会静默忽略。当前操作参数不支持嵌套对象、数组或 null。
整数须为精确可表示的整数（绝对值不超过 `2^53-1`）。
声明转换预算为 16 KiB、调用参数预算为 8 KiB（按键和值原始字节加节点开销计，不是 JSON 转义后的长度）。
这些限制不改变通用 `json` API 的能力。

可直接联调的完整 Lua 示例见 [10-voice-actions.lua](../tests/miniapp/10-voice-actions.lua)。

### 退出小应用：`ls.built_in.miniapp_exit`

退出当前小应用并返回桌面。无需参数，`arguments` 可省略或传空对象 `{}`：

```json
{"name":"ls.built_in.miniapp_exit","arguments":{}}
```

设备将退出请求交给小应用运行任务，返回 `miniapp exit requested`，随后释放实例并
返回桌面。退出会结束当前语音交互，下次唤醒恢复正常交互模式。没有小应用运行时
返回成功结果 `no miniapp is running`。`arguments` 为 `null`、非对象或含有任何
字段时均报错。退出不清除应用持久化存档，也不用于退出普通语音对话。

## 通过 ADB 本地调试

`CONFIG_MINIAPP_ADB_DEBUG` 默认启用，需要 CherryUSB ADB，
与完整的 `CONFIG_ADB_SYNC` 文件服务互斥。正常构建即可使用：

```sh
./build.sh -S ./apps/arcs-mini -DBOARD=arcs_mini
```

将对应板型的固件烧录到设备，等待启动完成、进入桌面后，每次上传一个 Lua 文件即可直接启动：

```sh
adb push ./miniapp.lua /miniapp/demo.lua
```

- `/miniapp/<id>.lua` 是虚拟入口，不需要文件系统。`id` 为 1–121 个 ASCII
  字母、数字、连字符或下划线；实际应用 ID 为 `local:<id>`。
- 相同路径对应同一个应用存档；每次上传都会重新启动，包括源码未改变时。
  版本号由源码 MD5 生成。本地应用与云端应用共用运行器，成功安装后替换当前实例。
- 上传成功表示脚本已通过校验并启动；语法或启动错误通过 `adb push` 返回，
  仍可用的旧实例会保留。启动成功后的运行错误沿用正常的小应用错误处理。
- 源码上限仍由设备配置决定，默认 64 KiB。源码仅保存在内存中，重启后不自动恢复；
  应用通过存储 API 保存的数据遵循原有的持久化和过期规则。
- 可通过 `adb pull /miniapp/miniapp.lua ./local.lua` 拉取当前运行应用的原始 Lua，
  包括云端安装的应用。没有应用运行时返回不存在；退出后不能开始新的拉取。
  拉取开始后固定源码快照，应用替换或退出不会导致文件内容混杂。
- 每条命令仅支持一个文件，不支持目录上传、列目录或其他拉取路径。
  传输中断会释放接收缓存；收到完整文件并开始安装后，断开 ADB 不撤销安装。
- 启用调试入口时额外保留活动应用的源码，最多 64 KiB；拉取共享只读快照，
  不复制整份源码。替换过程中旧源码在最后一个拉取连接关闭后释放。
- 接收缓存按源码大小在 PSRAM 中申请，安装任务使用临时的 4 KiB PSRAM 栈。
  Lua 实例仍受原有堆上限约束，替换时需容纳新旧两个实例；内存不足会返回失败。

不需要本地调试入口时，可通过 `-DCONFIG_MINIAPP_ADB_DEBUG=n` 关闭。

## Lua 基础接口

API 2 提供下述基础接口；API 3 增加系统时间与存档，API 4 增加 HTTP、JSON 与文本播报，
API 5 增加在线音频、语音操作回调和多声部音效合成。
新版本保留旧接口，应用按目标 `runtime.lua_sdk.version` 和能力对象选择接口。

仅接受 UTF-8 Lua 文本，不接受字节码。默认源码上限 65,536 字节，每个实例堆上限
393,216 字节，以能力响应为准。`id`、`version` 最长 127 字节，按 UTF-8 字节计。

| 接口 | 参数与约束 |
| --- | --- |
| `app.api_version` | API 版本整数 |
| `app.width`、`app.height` | 启用屏幕时的画布尺寸，默认 240 × 240 |
| `screen.begin(rgb[, options])` | 清空绘制列表并设置背景，颜色使用 `0xRRGGBB`；`ordered=true` 时按调用顺序叠放 |
| `screen.rect(x, y, w, h, rgb[, options])` | 矩形，尺寸为正，必须完全位于画布内；支持圆角、边框及透明度 |
| `screen.text(text, x, y, rgb[, options])` | 每帧最多 16 段文字，每段最多 63 字节，固定 16 像素字体；支持宽度、对齐、透明度 |
| `screen.present()` | 提交当前帧；中间帧合并，跟随显示刷新周期（默认 30 ms），相同画面不重复刷新 |
| `led.on("status")`、`led.off("status")` | 控制状态灯 |
| `led.blink("status", on_ms, off_ms)` | 亮、灭时长各为 10–5,000 ms |
| `buzzer.play(hz, ms)` | 100–5,000 Hz、20–3,000 ms；异步队列最多 4 项；固定合成幅度，再随系统音量调节；无返回值，繁忙时允许丢弃 |

### 多声部音效合成（API 5）

API 5 设备公布 `runtime.lua_sdk.synth` 能力时才可使用。`synth.play(score[, options])`
接受 1–256 个音符，乐段最长 60 秒，采样率 16 kHz，最多 4 个声部。音符字段为：

- `at`：相对乐段开始的毫秒数，0–60,000；
- `ms`：持续时间，20–3,000；
- `hz`：频率，50–5,000；
- `voice`：声部 1–4，同一声部的音符不能重叠；
- `wave`：`square`、`triangle` 或 `noise`；
- `volume`：0–100 的声部音量。

选项 `{loop=true}` 会循环最后一个音符结束形成的乐段。`synth.play()` 返回布尔值，
表示乐段是否被接收（安装验证阶段暂存，安装成功后入队），不保证最终播放成功；
队列满、应用退出或播放器未初始化时返回 `false`。参数格式错误、同声部重叠、
超过额度或内存不足时抛出 Lua 错误。`synth.pause()` 和 `synth.resume()`
暂停或恢复当前乐段；语音会话、TTS、闹钟和其他提示音抢占时保存播放位置，资源释放
后继续。退出或替换应用会清理合成任务，不会在新应用中恢复旧乐段。

合成器与 `buzzer` 共用 FIFO：最多 4 个等待项，另有 1 个正在播放的项；不混播多个
乐段。循环乐段持续占用播放位置，后续乐段和蜂鸣器等待到应用退出或替换后一起
被清理；暂停不会跳过循环乐段。连续交互音效应使用有限乐段，避免循环。

生成端接入时应读取 API 版本和设备公布的 `runtime.lua_sdk.synth` 能力对象，
确认支持的波形及额度，不能仅凭主 API 版本判断合成器可用。
无合成能力时降级为旧蜂鸣器或无声反馈；`synth.play()` 被拒绝时跳过本次音效，
禁止在 Lua 回调里忙等重试。

### 扩展绘图

API 4 及以上设备公布 `runtime.lua_sdk.graphics` 能力时才可使用扩展绘图。`graphics` 中的
`max_shapes`、`max_texts`、`text_max_bytes` 是实际设备预算；本版本分别为 256、16、63。
矩形、圆、线和圆弧共享图形额度，文字单独计数。预算是每帧上限，不保证满载帧率。

- `screen.circle(cx, cy, radius, rgb[, options])`：实心圆或圆环。
- `screen.line(x1, y1, x2, y2, rgb[, options])`：线段。
- `screen.arc(cx, cy, radius, start_deg, end_deg, rgb[, options])`：圆弧。

公共选项 `opacity` 为 0–255，默认 255。矩形和圆支持 `fill`（默认 true）、
`border_width`（0–64，默认填充时 0、不填充时 1）、`border_color`（默认 rgb）；
矩形额外支持 `radius` 圆角半径。线和圆弧支持 `width`（1–64，默认 1）和
`rounded`（默认 false，控制端点）。圆弧线宽不得超过半径。
圆/弧半径为 1 至画布宽高较大者，中心或线段端点可在各轴 `-画布尺寸` 至
`2 × 画布尺寸` 范围内，超出画布的部分裁剪。圆弧角度为 0–360，0° 在右侧，
顺时针增加；起止相同为空弧，0→360 为整圆，可跨越 0°。

文字选项：`width` 为文本框宽度（1 至画布剩余宽度），支持自动换行；
`align` 为 `left`、`center`、`right`，居中和右对齐必须指定 width；
`opacity` 同上。不传选项保持原有字体和布局。

`screen.begin(rgb, {ordered=true})` 按调用顺序绘制图形和文字。省略选项保留
旧应用的图形在下、文字在上的行为。局部更新会重绘变化范围内的所有相交内容，
移动或删除半透明图形不会留下残影。逻辑 `on_tick` 仍保持约 20 ms，
不保证每次 tick 提交的画面都独立显示。

未启用的硬件接口不注册相应 Lua 全局表。功能键未启用时不产生脚本按键回调，长按退出仍保留。

`on_tick(dt_ms)` 必须定义，正常调度间隔约 20 ms。可选回调为 `on_start()`、
`on_button_click(button_id)`、`on_audio_state(state)` 和 `on_exit()`，目前按钮 ID 为
`function`。在线音频状态变化时调用 `on_audio_state(state)`，其参数与
`audio.get_state()` 返回值相同。
启用屏幕时，启动阶段（源码执行、`on_start`、第一次 `on_tick`）必须至少提交一帧。

```lua
local clicks = 0
local function draw()
  screen.begin(0x101820)
  screen.text("Clicks: " .. clicks, 16, 16, 0xffffff)
  screen.present()
end
function on_start() draw() end
function on_tick(dt_ms) end
function on_button_click(button_id)
  clicks = clicks + 1
  draw()
end
```

允许基础函数及 `table`、`string`、`math`、`utf8` 库。不开放文件系统、原始套接字、系统命令、动态加载、
协程、调试库、`pcall`、`xpcall`、`setmetatable` 或手动 GC。
Lua 执行受指令预算和检查点耗时限制；它不提供抢占 C 库函数的硬实时隔离。
源码执行默认预算为 500,000 条指令／150 ms，回调为 250,000 条指令／500 ms。回调时间预算允许偶发的数据处理超过 tick 间隔；不代表每帧可以持续占用这么长时间。
支持 FreeRTOS 运行统计的设备结合任务累计运行时间检查耗时，避免射频校准等任务抢占导致误终止。
统计在任务切换时更新，因此耗时检查是近似值；指令预算独立限制脚本自身的计算量。
超过预算、堆耗尽或回调异常时终止对应实例；候选启动失败保留当前实例。

蜂鸣使用系统音量；TTS 或闹钟播放期间跳过蜂鸣，闹钟或新 TTS 到来时中断当前蜂鸣。队列满时丢弃新增蜂鸣，不阻塞脚本，也不导致小应用退出。
闹钟期间暂停脚本 tick 和按键输入，提醒结束后继续原实例，按键优先用于闹钟操作。

## 在线音频（API 5）

| 接口 | 说明 |
| --- | --- |
| `audio.play(url)` | 异步替换当前音频，返回是否接受请求；HTTP(S) URL 最长 511 字节，不允许空白、控制字符和内嵌 NUL |
| `audio.pause()` | 用户主动暂停，语音交互结束后也保持暂停 |
| `audio.resume()` | 解除用户暂停；有语音交互、TTS 或闹钟时，等待其结束再恢复 |
| `audio.stop()` | 停止并取消待播请求，后续恢复不会重播；需重新调用 `play` |
| `audio.get_state()` | 返回 `idle`、`loading`、`playing`、`paused`、`interrupted` 或 `error`，可在 `on_tick` 中查询 |

`idle` 表示无音频、主动停止或自然播放完成；`loading` 表示等待缓冲；`paused` 表示用户主动暂停；
`interrupted` 表示唤醒、语音交互、TTS 或闹钟临时抢占；`error` 表示请求、联网、格式或解码失败，
保持到下一次 `play` 或 `stop`。
控制接口不等待网络或解码完成，`play` 返回 true 不表示已经开始发声。
在首次加载完成前暂停会取消本次加载，保留 URL；恢复时重新加载。已开始播放后的暂停/恢复
使用播放器原有的暂停/继续接口，不重新打开 URL。
同一应用只有一路音频，使用系统音量，不进入普通歌单、不自动切歌或循环。
静态文件、连续 MP3 直播和 M3U8/TS AAC 直播直接交给系统播放器处理；直播暂停后恢复的
位置由直播服务及播放器缓冲决定，不保证回到直播的当前时刻。

唤醒/语音交互期间自动暂停，整轮交互和 TTS 结束后恢复。闹钟同样优先；手动暂停不会被
这些恢复流程撤销。短促蜂鸣仍沿用音频焦点优先级，结束后恢复在线音频。
退出、脚本异常或成功替换应用时停止并取消音频；旧请求的完成/错误不会改变新应用状态。
网络断开或云端重连时保留小应用页面，应用可显示播放错误并在联网后重新调用 `audio.play`。
启动阶段的播放/暂停/停止仅记录候选应用的最终意图，安装成功后执行；安装失败保留旧应用音频。

```lua
function on_start()
  assert(audio.play("https://lhttp.qtfm.cn/live/339/64k.mp3"))
  screen.begin(0x101820)
  screen.text("Radio", 16, 16, 0xffffff)
  screen.present()
end
function on_tick(dt_ms)
  local state = audio.get_state()
  -- 根据状态更新画面；error 后可由用户操作重新 play。
end
function on_button_click(id)
  if audio.get_state() == "paused" then audio.resume() else audio.pause() end
end
```

参考电台与设备验收步骤见 `tests/miniapp/08-online-audio.lua` 和测试集说明。

## 系统时间与存档

API 3 保留 API 2 接口，增加以下接口，API 4 继续支持：

- `clock.now()`：UTC Unix 时间戳，整数秒。
- `clock.localtime()`：设备本地 `{year, month, day, hour, min, sec, wday, utc_offset}`；星期从 0（周日）开始，偏移单位为秒。
- 未校时均返回 `nil`，时钟类应用应显示等待状态并在后续 tick 重查。不要将系统启动的默认日期或 tick 累加值当作真实时间。
- `storage.load()`：读取当前安装包 ID 的存档表；不存在、过期或不可用时返回 `nil`。
- `storage.save(data[, ttl_seconds])`、`storage.clear()`：成功为 `true`，失败为 `false, reason`。

存档表最多 32 项，键为 1–32 UTF-8 字节字符串，值为有限数字、布尔值或最多 256 UTF-8 字节字符串；字符串不含 NUL，不支持嵌套。序列化总大小以 capabilities 中 `runtime.lua_sdk.storage.max_bytes` 为准。
当前实现每份最多 1,024 字节，最多 4 份存档；默认有效期 7 天、最长 30 天。同一存储槽至少间隔 10 秒写入，退出或清除不重置间隔。满额返回 `storage_full`，不会驱逐其他未过期应用的数据。原有分区表不变，仅使用固定数量的独立 KV 键。

有效期从保存成功算起，读取不续期，过期数据访问时回收。未校时暂不恢复或保存数据，已有存档不因此删除；显式清除仍可执行。常见失败包括 `clock_unavailable`、`rate_limited`、`too_large`、`invalid_data`、`invalid_ttl`、`storage_full`、`io_error`、`no_memory`、`unavailable`。参数种类错误会触发 Lua 异常。

启动候选阶段暂存写入，仅在替换成功后尝试落盘，落盘失败记日志；此阶段 `true` 仅表示接受暂存。正常运行阶段 `true` 表示写入成功。启动失败不会提交暂存修改，退出中的旧实例不能覆盖候选实例的存档。

应用自行兼容旧数据，不强制存档版本号。读取过存档的实例发生 Lua 异常时清除该 ID 的存档，异常退出禁止再保存；不自动重试，下次从空数据启动。底层存储故障可能使清除失败。存档只用于可丢失的进度、设置等信息。

## HTTP、JSON 与文本播报（API 4）

本节说明 API 4 接口契约。可用性和额度取自前述能力响应，不从 API 版本单独推断
HTTP 或 TTS 可用。联网请求异步执行，不应阻塞首帧绘制或 tick；先显示可操作的界面，
再根据结果更新状态。HTTPS 使用固件的 TLS 配置，接口不保证服务器证书校验。

### HTTP 与 JSON

| 接口 | 约定 |
| --- | --- |
| `http.request(options)` | `url` 必填；`method` 默认 GET，允许值见 `http.methods`；可选 `headers`、`body`、`timeout_ms`、`max_response_bytes` |
| `http.get(url[, options])` | GET 简写，options 可指定 headers、timeout_ms、max_response_bytes |
| `http.cancel(request_id)` | 取消当前实例的指定请求，返回是否成功取消 |
| `json.encode(value)` | Lua 值编码为 JSON；失败返回 `nil, reason` |
| `json.decode(text)` | JSON 解码为 Lua 值；失败返回 `nil, reason` |
| `json.null` | 表示 JSON null，避免以 nil 表示时丢失字段或数组元素 |

请求体仅用于 POST；请求头为字符串键值表，不允许 NUL 或换行。
省略超时及响应大小时分别使用能力中的 `default_timeout_ms` 和 `default_response_bytes`，
显式值不得超过 `max_timeout_ms` 和 `max_response_bytes`。不自动重试或跟随重定向。

请求返回数值 ID，结果交给可选的 `on_http_response(request_id, response)`。
未定义此回调时，运行时丢弃响应并释放请求额度，不终止应用。收到完整 HTTP
响应时提供 `{status, content_type, body}`，HTTP 4xx/5xx 也属于完整响应；传输失败时
提供 `{error = {code, message}}`。应用应分别检查传输错误、HTTP 状态和业务数据。

```lua
local pending
function on_button_click(button_id)
  if button_id == "function" and not pending then
    pending = http.get("https://example.com/data", {max_response_bytes = 1024})
  end
end
function on_http_response(id, response)
  if id ~= pending then return end
  pending = nil
  if response.error or response.status ~= 200 then return end
  local data, err = json.decode(response.body)
  if type(data) ~= "table" then return end
  -- 按服务约定检查业务字段后更新界面。
end
```

示例仅展示请求流程，应用还需定义基础生命周期回调；实际 URL 和数据格式由服务提供方确定。
JSON 对象使用字符串键，数组使用从 1 开始的连续整数键；不支持循环引用、稀疏或混合键表、
函数和非有限数值。JSON false 是合法结果，不能用 `if not value` 判断解析失败。

### 文本播报

`tts.speak(text)` 接收非空 UTF-8 纯文本，使用设备当前发音人与系统音量，返回播报 ID。
文本长度及请求额度取自 `runtime.lua_sdk.tts`。结果通过可选的
`on_tts_result(speech_id, result)` 回调交付；需要显示播报状态或处理失败时应实现回调。
未定义此回调时，播报仍正常执行，结果被忽略并释放请求额度，不终止应用。

- `completed`：音频播放完毕。
- `failed`：请求或播放失败，可附带 `error = {code, message}`。
- `interrupted`：被唤醒、闹钟等系统行为中断。

对话占用时异步返回 `failed`，错误原因为 `busy`；不抢占对话，也不自动排队。
`tts.cancel(speech_id)` 只取消当前实例的对应播报，不能停止对话或闹钟音频。

### 异步生命周期

请求 ID 用于匹配结果和取消，不代表请求成功。普通参数错误、额度超限和繁忙通过结果
回调报告；回调在 Lua 任务中串行执行，每个请求最多交付一次最终结果。
成功取消后不再回调，已完成或重复取消返回 false。

候选启动阶段的请求只在安装成功后发出；候选失败不能产生网络或播报副作用。
闹钟冻结期间不执行 Lua 回调，恢复后交付已完成的结果；待交付结果仍占用请求额度。
退出或替换实例会取消其请求，旧结果不会交付给新实例。

## 小应用语音模式

设备在每个 `start` 帧生成时读取当前运行状态。小应用活跃时添加：

```json
{
  "action": "start",
  "params": {
    "nlu_properties": {
      "custom": {
        "mode": "miniapp",
        "miniapp": {"id": "<app-id>", "version": "<app-version>"}
      }
    }
  }
}
```

退出或运行异常结束后，后续 `start` 帧不携带小应用的 `mode` 和 `miniapp` 字段。
断线重连后的会话同样读取当前状态，不复用上一次会话的模式。
安装失败并保留旧应用时，继续携带 `miniapp`。

云端收到此模式后，处理重新生成、修改、切换、询问玩法，以及应用已声明的语音操作；应在产生业务 TTS 或调用工具前
过滤播放音乐、闲聊及其他无关意图。对“暂停倒计时”等应用内操作，先调用 `ls.built_in.miniapp_describe`，
确认 `running=true`，依据 `actions` 选择操作并调用 `ls.built_in.miniapp_action`；不得凭空猜测操作名或参数。
端侧仅放行能力查询、安装、退出、描述和操作小应用的 MCP 调用，拦截音乐播放；主动推送 TTS 正常播放。
已设闹钟仍由本地调度触发，不受 MCP 白名单影响。

`mode` 是会话开始时的快照，不会追溯修改已发出的 `start` 帧。
模式切换或替换活动应用时停止本地上行并清理待续聊状态，保留当前回复 TTS；再次唤醒后按新模式开始会话。
长按退出和运行异常退出均结束当前交互，避免回到桌面后继续沿用小应用模式。
实体退出会使正在下载的旧安装失效。若云端在退出之后才新发出旧会话的安装调用，
现有六参数协议无法单独判断其是否过期，云端还需根据会话取消状态停止下发。

## 验证范围

实现需验证参数类型及长度边界、下载中断、错误长度或 MD5、内存不足、Lua 语法及回调错误、
无限循环、界面打开失败、相同版本复用、替换失败保留旧应用，以及退出后的资源释放。
设备验证还需覆盖单击与长按、全屏显示、蜂鸣音量、音乐停止、相关语音交互、无关意图拦截、TTS 保留及闹钟响铃。
