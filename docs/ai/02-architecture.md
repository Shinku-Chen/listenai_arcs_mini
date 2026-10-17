# 02 架构：目录、启动流程、消息总线

## 1. 目录地图

```
apps/arcs-mini      CP 固件（业务主体，日常改这里）
apps/remote-ap      AP 固件（算法）
apps-ui/            LVGL 界面：apps/llm（助手页 / 扫码配网 / 设置页）
src/
  category/         品类差异实现（comm 为通用）
  framework/        voice_msg.* —— 全局消息总线
  middleware/       硬件抽象（battery / ble / button / display / kv / power / usb…）
  server/lschat/    云端会话、唤醒
  shell/            调试命令行（SHELL_EXPORT_CMD 注册）
  system/platform.c 平台初始化（WiFi / BLE 在这里起来）
  utils/            事件、配置、编解码
arcs-sdk/           SDK：HAL / 驱动 / 组件 / 第三方库 / 板型（boards/<board>）
                    （samples/ 未入镜像，需要时去上游仓取；见 06-upstream-sync.md）
                    镜像里 CONFIG_MINIAPP=n —— 自定义固件不用小程序运行时
modules/            项目级模块：ebus（消息）、lschat、lisaui
res/<board>/        板型资源 + partition_table.json（mini3 另有 ap/boot）
docs/               上游文档（miniapp.md 等）；AI 文档在 docs/ai/
```

## 2. 启动流程（`apps/arcs-mini/main.c`）

```
上电/复位
 └─ 启动代码：内存初始化 → RTOS 起来 → SYS_INIT 分级初始化（消息系统在 main 之前就绪）
     └─ main()
         ├─ 打印版本、boot_watchdog_feed()
         ├─ voice_msg_sub(...)  订阅关心的消息
         ├─ service_led/volume/brightness/button/camera/..._init()
         ├─ lisa_ui_init()  （CONFIG_APPLICATION_UI）
         ├─ 功能模块 init（各自 Kconfig 门控）
         └─ while(1) { boot_watchdog_feed(); vTaskDelay(100ms); }
```

**平台侧初始化**（BLE / WiFi）在 `src/system/platform.c`，早于 `main()`：

```c
if (ipc_ready) {
    network_probe_init();
    sys_wifi_init();
    lisa_bluetooth_init(NULL);   /* 传回调的话，回调在 BT 任务里执行 */
    app_ble_netcfg_init();       /* 注册连接/断开/配对回调、BLE 配网 */
}
```

## 3. 消息总线（本工程的核心接线方式）

```c
voice_msg_pub(msg_id, data, len);              /* 发布 */
voice_msg_sub(msg_id, cb, user_data);          /* 订阅 */
```

主链路：

```
WiFi 拿到 IP → VOICE_MSG_WIFI_IP_GOT
  → network_probe_start() → VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS
  → voice_cloud_connect() → VOICE_MSG_CLOUD_CONNECTED
  → VOICE_MSG_CLOUD_* （IAT / TTS / FINISH …）
按键 → service_button.c → VOICE_MSG_BUTTON_CHANGE（payload: voice_msg_button_evt_t）
播放器 → 订阅 CLOUD_TTS_URL / CLOUD_AUDIO_ITEM / WAKEUP_* / WIFI_DISCONNECTED
```

**新模块的标准写法**：初始化（`SYS_INIT` 或 `main()` 里）+ 订阅若干 `VOICE_MSG_*` +
自己的 LVGL screen（`lisa_ui_nav_scr_add()` / `lisa_ui_nav_scr_nav_to(id)`）。
业务不要写进主循环轮询。

## 4. 调试入口

| 工具 | 用途 |
|---|---|
| `SHELL_EXPORT_CMD(...)` | 注册 `adb shell <cmd>`，暴露状态/开关（例：`ic show|hide|stat`） |
| `adb shell threads` / `heap` | 线程与堆现状（判断泄漏、栈不足） |
| `adb shell kv` | KV 读写（存档、设备 id、音量等） |
| `adb shell device set_pid/set_sid` | 绑定云端应用 |
| `adb shell recovery` | 进烧录模式 |
| `riscv64-unknown-elf-addr2line -e build/arcs-mini -a <addr>` | 崩溃地址还原 |

## 5. 配置（Kconfig）

- 应用级在 `apps/arcs-mini/prj.conf`；板型专属在 `apps/arcs-mini/<BOARD>.conf`（会被自动合并）。
- 构建后**实际生效值**看 `build/.config`，别凭 prj.conf 猜。
- 新功能一律加自己的 `CONFIG_<FEATURE>`（默认 `n`），在 `prj.conf` 显式打开——
  这样别人能一眼关掉，也方便提上游。
