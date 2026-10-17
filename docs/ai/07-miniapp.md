# 07 小程序（Lua）与固件的关系

本仓是**固件**仓。小程序（LingClaw 小应用）是另一条线：

| | 小程序（Lua） | 固件（本仓） |
|---|---|---|
| 代码在哪 | `https://github.com/Shinku-Chen/ling` 的功能分支 | 这里 |
| 运行形态 | 裸 Lua，云端下发或 adb `push` 到 `/miniapp/<id>.lua`，只在 PSRAM 里跑 | native C + LVGL，编进镜像 |
| 改一次成本 | 秒级（改完重新 push 即可） | 分钟级（编译 + 烧录 + 重启） |
| 能碰硬件 | 只有运行时下发的能力表 | 全部（BLE / 音频 / GPIO / 摄像头） |

## 什么时候必须写固件

- 需要**新增运行时能力**（例如给沙箱加一个新的 `audio.*` / BLE 接口）；
- 需要**直连外设**（引脚、I2C、摄像头、原始 socket）；
- 需要**实时性**（音频链路、按键边沿、协议栈）。

纯 UI、联网取数、可热更的业务优先写小程序——改一次的成本差两个数量级。

## 设备侧契约在哪

- 上游文档：`docs/miniapp.md`
- 完整手册（运行时上限、编码约定、调试闭环、云端下发边界）：`Shinku-Chen/ling` 的
  `docs/development/miniapp-runtime.md` 与 `coding-conventions.md`
- 沙箱硬限制（改固件时也别违背）：
  - 单文件 ≤ 65,536 字节；单实例 Lua 堆 ≤ 393,216 字节
  - 每帧 ≤128 个矩形、≤8 段文字、每段 ≤63 字节；字体固定 16 px
  - 没有 `require` / `pcall` / `io` / `os`；参数非法会**直接终止小应用**
- 存档：写入间隔 ≥10 秒、总量 ≤1 KiB、未校时不保存

## 固件侧怎么支持小程序

- 能力开关都在 `CONFIG_MINIAPP_*`（见 `apps/arcs-mini/miniapp/Kconfig`）；
- Lua API 的实现在 `apps/arcs-mini/miniapp/miniapp_runtime.c`；
- 新增能力时**同步更新** `get_device_capabilities` 上报（`mcp_tools/mcp_tool_device_capabilities.c`）
  和 `docs/miniapp.md`，否则云端与小程序看不到。
