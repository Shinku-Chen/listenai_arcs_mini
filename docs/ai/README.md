# AI 开发文档（arcs_mini 固件）

这是**给 AI 与新人看的入口索引**，配合仓库根目录的 `AGENTS.md`（上游自带规则）使用。
本目录是镜像仓的个人增强层：上游同步时本目录原样保留，不会被打回。

> 上游：`https://cloud.listenai.com/CSKG836746/arcs-sdk/public/arcs_mini`（公开仓，当前版本 3.0.3）
> 镜像机制见 [06-upstream-sync.md](06-upstream-sync.md)。

## 按任务找文档

| 我要…… | 读这份 |
|---|---|
| 第一次跑起来（装工具链 → 编译 → 烧进去 → 看日志） | [01-quickstart.md](01-quickstart.md) |
| 搞清工程结构、启动流程、消息总线怎么接线 | [02-architecture.md](02-architecture.md) |
| 烧录、换板型、救砖、恢复出厂 | [03-flashing.md](03-flashing.md) |
| 真机黑屏 / ADB 掉线 / 起不来 | [04-pitfalls.md](04-pitfalls.md) |
| 写代码该守什么规矩（改哪里、开关、内存、LVGL/BLE/按键） | [05-coding.md](05-coding.md) |
| 想知道这个仓怎么跟上游同步、哪些文件被排除 | [06-upstream-sync.md](06-upstream-sync.md) |
| 写小应用（Lua） | [07-miniapp.md](07-miniapp.md) → 详细手册在 `Shinku-Chen/ling` |
| 端侧 MCP 工具 | `.agents/skills/mcp/SKILL.md` |
| 官方 SDK 手册 | 在线：<https://docs2.listenai.com/arcs-sdk/v0.1.0/zh/html/get_started.html>；本地：`arcs-sdk/docs/zh/` |

## 三条硬事实（先记住，能省一天）

1. **板型只有 `arcs_mini3`**（LS2663）。同系列的 `arcs_mini` 是另一块板（LS2684），引脚与 boot/ap 都不同，
   **刷错 = 黑屏 + USB 不识别 + 反复重启**。构建必须 `-DBOARD=arcs_mini3`，资源取 `res/arcs-mini3/`。
2. **构建通过 ≠ 真机通过**。BLE、按键边沿、屏幕时序、音频这些只有真机能确认；
   本项目已经在"能编译但在真机上开机即死"上栽过（见 [04-pitfalls.md](04-pitfalls.md)）。
3. **不要改 `apps-ui/apps/llm/**`**（助手页 / 扫码配网 / 设置页）。新功能做独立 screen，走导航管理器进出。

## 交付汇报模板

```text
Build:         PASS / FAIL / NOT RUN   （-DBOARD=arcs_mini3，附 FLASH/PSRAM/SRAM 百分比）
Static checks: PASS / FAIL / NOT RUN
Device run:    PASS / FAIL / NOT RUN   （烧录 + 真机验收清单）
Unverified:    还需人工/真机确认的项
```
