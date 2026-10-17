<!-- ai-docs:begin -->
## AI 开发规范在另一个仓库（本仓只做镜像）

本仓是**上游 arcs_mini 的只读镜像**：只同步上游，不放个人文档。
**改本仓代码前，先读 `Shinku-Chen/ling` 的固件文档**（那边是唯一真相源）：

| 我要…… | 去 ling 读 |
|---|---|
| 板型红线、内存预算、分区地址、BLE/LVGL/按键约定 | `docs/firmware/conventions.md` |
| 开发流程：分支 → 构建 → 烧录 → 真机验收 → 提交 → 回滚 | `docs/firmware/process.md` |
| 目录地图、启动流程、`voice_msg` 消息总线 | `docs/firmware/architecture.md` |
| 真机踩坑集（SRAM 溢出 / ADB_LIBUSB / 冷复位 / GATT 注册…） | `docs/firmware/pitfalls.md` |
| 固件仓自带手册摘要（构建、烧录三模式、日志、skills） | `docs/firmware/arcs-mini-repo.md` |
| 本仓（镜像）机制：快照同步、剔除 samples、`CONFIG_MINIAPP=n` | `docs/firmware/upstream-mirror.md` |
| mini3 引脚定义（板级事实） | `docs/hardware/board-arcs-mini3.md` |
| 小程序（Lua）开发 | `docs/development/` |

仓库：<https://github.com/Shinku-Chen/ling>

**本仓三条硬规则**（同步时由脚本自动维护，不要手改）：
1. `arcs-sdk/samples/` 不入镜像，因而 `apps/arcs-mini/prj.conf` 是 **`CONFIG_MINIAPP=n`**；
   要小程序能力时从上游取回 `samples/subsys` 再打开。
2. 本仓不放文档/规范：新东西一律写进 ling。
3. 同步用 `tools/sync-upstream.ps1` / `.sh`（Actions 每周一自动跑），不要手工改上游文件。
<!-- ai-docs:end -->
