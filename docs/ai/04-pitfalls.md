# 04 踩坑集（都是真机实测，不是推测）

## 1. 构建：`build.ps1` 被 PowerShell 判成失败

**现象**：CMake 输出 `-- Configuring done` 后脚本报错退出（exit 1），什么都没编。

**原因**：CMake 会把 Kconfig 的警告 `warning: default on the choice symbol APP_USB_ROLE_DRP … will have no effect`
写到 **stderr**；`build.ps1` 顶部有 `$ErrorActionPreference = "Stop"`，PowerShell 把
NativeCommandError 当致命错误。

**处置**：直接调 cmake + ninja（行为等价）：

```powershell
$env:LISTENAI_TOOLS_PATH="C:\Users\<you>\.listenai\listenai-tools"
$env:NUCLEI_TOOLCHAIN_PATH="C:\Users\<you>\.listenai\gcc"
$env:ARCS_BASE="<repo>/arcs-sdk"
& "$env:LISTENAI_TOOLS_PATH\cmake\bin\cmake.exe" -B build-x -G Ninja -S apps/arcs-mini `
    -D CMAKE_MAKE_PROGRAM="$env:LISTENAI_TOOLS_PATH\ninja\ninja.exe" `
    -DBOARD=arcs_mini3 -DBOARD_SEARCH_PATH="<repo>/boards"
& "$env:LISTENAI_TOOLS_PATH\ninja\ninja.exe" -C build-x -j 12
```

## 2. SRAM 只剩十几 KB：`region 'SRAM' overflowed`

**实测**（`arcs_mini3`，含对讲机功能）：FLASH 3.1 MB/8 MB（37%）、PSRAM 5.17 MB/5 MB（98.7%）、
**SRAM 157 KB/168 KB（91.3%）**。

新增静态大缓冲必然撞这个错：`region 'SRAM' overflowed by N bytes` /
`section .noinit will not fit in region SRAM`。

**处置**：大缓冲搬到 PSRAM——

```c
#include "sysutils.h"
static uint8_t big_buf[4096] __psram_bss__;
```

堆分配用 `pvPortMalloc` / `vPortFree`（FreeRTOS 堆线程安全）。不要用 `lv_mem_alloc` 跨线程分配。

## 3. `adb devices` 空，但 USB 节点在

**现象**：`Get-PnpDevice` 能看到 `USB\VID_0483&PID_0ADB\<chip-id>`（Status OK），
ADB 接口绑的是 **inbox WinUSB**（Service=WINUSB），但 `adb devices` 一直空。

**原因**：Google adb 默认后端按设备接口 GUID 找设备；inbox WinUSB 没带 ADB 的 GUID。

**处置**（不需要管理员、不改驱动）：

```powershell
$env:ADB_LIBUSB=1
adb start-server      # 后端在 server 里，起一次后续普通 adb 命令都能用
adb devices -l        # 6d7b101a252fcaa5 device product:arcs_mini3
```

## 4. cskburn 之后黑屏、USB 消失

**原因**：cskburn 写完 `Resetting...` 是**冷复位**，AON 里的热启动意图位被清掉，
boot 不拉 USB、屏幕不亮。看起来像刷砖，其实只差一次长按。

**处置**：长按功能键 3 秒。**软复位**（`adb shell reboot` / `adb shell recovery` 之后）不受影响。

## 5. BLE 自定义 GATT 服务不能在主任务里注册

**现象**：新加的 BLE 服务代码编译通过，真机**开机即黑屏 + USB 完全消失**（adbd 也没起来）。

**原因**：`ic_app_init()`（主任务）里直接 `ble_gatt_user_register()`，此时协议栈未就绪 → 挂住。

**处置**：注册放到 BLE 初始化完成回调里（该回调由 **BT 任务**调用）：

```c
static void ble_ready_cb(uint16_t status) {
    if (status == 0) { ic_link_init(); }   /* 在这里注册 GATT 服务 */
}
lisa_bluetooth_init(ble_ready_cb);          /* platform.c */
```

## 6. 主功能键拿不到"按下 / 松开"边沿

**现象**：想做"按住说话 / 长按录音"，但 `VOICE_MSG_BUTTON_CHANGE` 只给 `CLICK…CLICK / LONG_HOLD`。

**原因**：`apps/arcs-mini/services/service_button.c` 里对主功能键做了过滤
（`service_button_power_key_accepts_action`），只放行点击序列与 `LONG_HOLD`。

**处置**：不要放宽共享过滤器（会影响助手页、配网、关机）。在**驱动回调**里直接分发原始边沿：

```c
static void btn_event_callback(lisa_btn_event_t event, uint8_t btn_id, void *user)
{
#ifdef CONFIG_INTERCOM
    ic_app_on_raw_button(event, btn_id);    /* 自己用按下/松开 */
#endif
    publish_button_event(btn_id, event, true);
}
```

配套要求：任何"按住"类功能在前台时必须**消费掉 `LONG_HOLD`**，否则用户说话时设备直接关机
（长按 = 关机是系统行为）。

## 7. BLE 握手要等 CCCD 打开

手机没订阅 notify 时，设备发出去的帧**全部会被丢掉**（发送环返回失败）。
握手（hello/status）必须放在 `CCCD 由关变开`的时刻，不能放在连接建立那一刻。

## 8. 广播包只有 31 字节

广播包已被 `flags + 名字 + 厂商数据` 占满（BLE 配网小程序依赖厂商数据）。
新增 128-bit 服务 UUID 之类要放**扫描响应**（`lisa_bt_get_scan_rsp_data` 是 weak 可覆盖），
安卓侧会把 adv 与 scan response 合并解析。

## 9. 串口抓日志：cskburn 之后要等一下

cskburn 烧完立刻开串口，可能抓到空文件或乱码——串口还没释放稳定。**先 sleep 3~5 秒**。

## 10. 设备"黑屏 + ADB 掉线"先怀疑休眠

空闲 30 s 降亮度、5 min hibernate（`home_enter_hibernate` 会挂起 USB 与 LVGL），
表现就是黑屏 + ADB 不应答 + 无日志。一次按键即可唤醒；跑小应用时固件会抑制休眠。
