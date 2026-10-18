# LEO 网络电台 · 功能固件

基于 LISTENAI ARCS-MINI（**arcs_mini3** / LS2663）的自定义固件：在原有语音助手之上，
把 [leo-radio](https://github.com/leo0183/leo-radio) 的城市网络电台做成了固件原生功能。

## 刷哪个文件

| 文件 | 用途 | 命令 |
|---|---|---|
| `*-app.bin` | **只刷 CP 应用**（日常升级，推荐） | 串口：`cskburn -C arcs -b 1500000 -s COMx 0x600000 *-app.bin`<br>ADB：`adb push *-app.bin /RAW/NAND/600000` |
| `*-full.lpk` | 整包（boot/ap/资源/app 全有），给官方 cskburn desktop 用 | 直接选 lpk |
| `*.ota.txz` | 走设备自己的 OTA 流程（需自己发布到产品下；本固件默认关掉了官方 app OTA） | — |
| `*.combined.hex` | 一次性烧全片（产线/救砖） | — |

- **板型必须是 `arcs_mini3`**。`arcs_mini`（LS2684）是另一块板，刷错会黑屏 + USB 不识别。
- 串口写入完成是**冷复位**：需要**长按功能键 3 秒**才开机。
- 只刷 app 分区（`0x600000`）不会动 boot/ap/资源/KV，原有 Wi-Fi 与设置保留。

## 功能与交互

- **开机 1.2 s 自动进入电台页并播放**（`CONFIG_RADIO_AUTO_START`）
- 四击进入/退出电台页；页内单击换台、双击播放/暂停、长按进设置菜单、三击逐级返回
- 五/六/七击：退出电台并进入 **BLE 配网**（沿用原有配网流程，未改一行）
- 台单：`ip-api.com` 定位城市 → `radio-browser.info` 按城市查台（含流派/编码/码率/语言），
  失败回退 6 个内置台；台单、城市、音量持久化在 KV
- 睡眠定时：关闭 / 15 / 30 / 60 / 90 分钟，倒计时直接覆盖左上角的频道号
- 播放状态只看大数字颜色：琥珀=在播 / 暗琥珀=连接中 / 灰=暂停

## 与官方固件的关系

- 本固件在 `prj.conf` 打开了 `CONFIG_OTA_DISABLE_APP_UPDATE=y`：**官方 app OTA 不会再覆盖它**
  （官方 app 镜像会把 `0x600000` 整块替换掉，自建功能会全丢）。资源包 OTA
  （提示音 / 唤醒词 / 表情）不受影响，可以继续收。
- 想回到官方固件：刷官方 `.lpk` 整包即可。

## 调试

```bash
adb shell "radio stat"        # 状态（fg/page/count/index/net/pb/vol + 台名/流派）
adb shell "radio list"        # 台单（名字 | url | 频率 | 流派 | 编码码率 | 语言 | 原名）
adb shell "radio key click"   # 模拟按键：click/double/triple/quad/five/long
adb shell "radio show|hide|next|play|pause|city <n>"
```


## 发版（打 tag 自动构建）

```bash
git switch feat/leo-radio && git pull
git tag -a leo-radio-v3.0.4 -m "LEO 网络电台 3.0.4"
git push origin leo-radio-v3.0.4          # 触发 release-leo-radio → 调用 master 的通用流水线
```

- 打包逻辑在 **master** 的 `.github/workflows/release-firmware.yml`（`workflow_call` 可复用）。
  本分支只有十几行的 caller，换新功能固件时复制它改 4 个 `with` 参数即可。
- 也可以 Actions → `release-leo-radio`（或 master 的 `release-firmware`）→ Run workflow 手动跑。
- Release tag 直接用打的 tag 名；`--latest` 会把它标成最新。

--- 

源码分支 `feat/leo-radio`；固件文档见 <https://github.com/Shinku-Chen/listenai_arcs_mini_doc>
的 `docs/firmware/radio.md`。
