# 05 编码约定

配合上游 `.agents/skills/coding/SKILL.md` 看；这里只列**最容易踩、代价最大**的那几条。

## 1. 改哪里：范围优先级

1. 优先改 `apps/<app>/`（应用工程）；
2. 需要多 app 复用时才改 `src/`（PDK 公共能力）；
3. **非必要不改 `arcs-sdk/`**；必须改时先说明原因、影响范围、替代方案。

## 2. 开关与注册

- 新功能必须有 `Kconfig` 开关（默认 `n`），在 `apps/arcs-mini/prj.conf` 显式打开。
- 新模块放 `apps/arcs-mini/<feature>/`，用 `listenai_library_named` + `listenai_library_sources`，
  并在 `apps/arcs-mini/{CMakeLists.txt,Kconfig}` 各挂一行。
- 初始化点：UI 相关的放 `apps/arcs-mini/main.c`；BLE/WiFi 相关的放 `src/system/platform.c`；
  两者都用 `#ifdef CONFIG_<FEATURE>` 包住，关掉时零代价。

## 3. 内存

- **AP SRAM 只剩十几 KB**（实测 91% 已用）。新增静态缓冲一律 `__psram_bss__`。
- PSRAM 也接近满（98.7%），加功能前先算账，别默认"还有 6 MB"。
- 跨线程分配用 `pvPortMalloc/vPortFree`；`lv_mem_alloc` 只在 LVGL 线程里用。

## 4. LVGL

- **不要改 `apps-ui/apps/llm/**`**（助手页 / 扫码配网 / 设置页）。
- 新界面做独立 screen：`lisa_ui_nav_scr_add(&scr)` + `lisa_ui_nav_scr_nav_to(id)`，
  `unique_id` 自己挑未占用的（例：90/91）；返回用 `lisa_ui_nav_scr_nav_default()`。
- 中文字体用 `lv_font_chinese_16`（`lisa_ui_fonts.h`），不要自带字库。
- **LVGL 不是线程安全的**：从 BT 任务 / shell 改界面必须 `lv_async_call(cb, arg)`，
  arg 放堆上、在回调里释放。

## 5. BLE

- 固件已初始化蓝牙（`platform.c`），**不要重复初始化**。
- SDK 的连接/断开/配对回调是**单槽位**；新功能要通过 `src/middleware/ble/app_ble_common.c`
  的既有回调**转发**进来，直接再注册会顶掉 BLE 配网。
- GATT 服务注册放 BLE 就绪回调（见 [04-pitfalls.md](04-pitfalls.md) §5）；
  通知用 `ble_gatt_srv_event_send(..., BLE_GATT_NOTIFY, hdl, ...)`，且必须等 `cb_event_sent`
  回来再发下一帧（协议栈不支持并发发送）。
- 握手/推送等 CCCD 打开之后再发（[04-pitfalls.md](04-pitfalls.md) §7）。

## 6. 按键

- 共享消息总线只放行点击序列与 `LONG_HOLD`；需要边沿就从驱动回调分发（§6）。
- 长按 = 关机是系统行为；"按住说话"类功能必须在前台消费 `LONG_HOLD`。

## 7. 日志与调试

- 统一 `LISA_LOGI/W/E(TAG, ...)`，`TAG` 用 `"模块.子模块"`（如 `ic.link`）。
- 需要现场验证的能力做 shell 命令（`SHELL_EXPORT_CMD`），例：`ic show|hide|stat|text <s>`。
- 状态类数据暴露 counters（收/发/丢/重同步），出问题时有数字看。
- 崩溃：`riscv64-unknown-elf-addr2line -e build/arcs-mini -a <addr>`。

## 8. 防御式编程

C 侧同样没有"异常兜底"：所有外部输入（BLE 写入、HTTP 响应、KV、时间）都要：
判空 → 判长度 → 判范围 → 再使用。参数非法或越界不会抛异常，会直接崩或静默跑错。

## 9. 提交

conventional commits + 中文：`fix(player): 修复唤醒提示音随机选择在特定数量下可能死循环的问题`。
正文写"为什么"和**实测数据**（内存百分比、真机验证结果）。原子提交，一个分支一件事。
