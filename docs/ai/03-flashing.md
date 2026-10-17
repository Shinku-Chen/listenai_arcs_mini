# 03 烧录：三条通道、分区、开发 Boot、救砖

## 1. 先确认板型与分区

分区表真相源是 `res/<board>/partition_table.json`。**mini3** 的地址：

| 分区 | 地址 | 文件 |
|---|---|---|
| boot | `0x000000` | `res/arcs-mini3/boot.bin` |
| ap | `0x040000` | `res/arcs-mini3/ap.bin` |
| tone | `0x100000` | `res/arcs-mini/tone.bin` |
| wake_word | `0x200000` | `res/arcs-mini/wake_word.bin` |
| emoji | `0x380000` | `res/arcs-mini/emoji.bin` |
| respak | `0x440000` | `res/arcs-mini/respak.bin` |
| **app** | **`0x600000`** | `build*/arcs-mini.bin` |
| ota | `0xA00000` | 暂存 5 MiB |
| kv | `0xF00000` | 1 MiB |

> mini3 的 tone / wake_word / emoji / respak **共用** `res/arcs-mini/` 里的文件（`res/arcs-mini3/` 只有 ap/boot/分区表）。

## 2. 通道一：ADB（首选）

```powershell
.\adb_download.ps1 -S res\arcs-mini3 app      # 只烧 CP app（日常改代码走这条）
.\adb_download.ps1 -S res\arcs-mini3          # 完整：ap + 资源 + app（不动 boot）
.\adb_download.ps1 -S res\arcs-mini3 boot     # 整包：额外烧 boot.bin
```

脚本会：检测 adb → 让普通模式设备 `recovery` → 等 `BOOT-*` 出现 → 按分区表推送 → 重启。
多设备时必须 `-s <serial>`；两台同型号时按 `transport_id` 区分。

手工等价（Git Bash 要 `MSYS_NO_PATHCONV=1`，本地路径用 Windows 形式）：

```bash
adb -s BOOT-xxxx shell recovery          # 进 recovery
adb -s BOOT-xxxx push build/arcs-mini.bin /RAW/NAND/600000    # 远端路径去掉 0x 与前导零
adb -s BOOT-xxxx shell reboot hard
```

## 3. 通道二：cskburn 串口（ADB 起不来时）

前提：串口线接到板子的烧录 UART（mini3 是 UART2 = PB06/PB07），或板载 CH340。

```powershell
.\tools\cskburn\cskburn.exe -C arcs -s COM10 -b 921600 -c        # 只探测（安全）
.\tools\cskburn\cskburn.exe -C arcs -s COM10 -b 921600 --verify-all `
  "0x0"      res\arcs-mini3\boot.bin `
  "0x40000"  res\arcs-mini3\ap.bin `
  "0x100000" res\arcs-mini\tone.bin `
  "0x200000" res\arcs-mini\wake_word.bin `
  "0x380000" res\arcs-mini\emoji.bin `
  "0x440000" res\arcs-mini\respak.bin `
  "0x600000" build\arcs-mini.bin
```

- 波特率：官方文档写 `1500000`；实测 `921600`（CH340）稳定，更高速率要先确认适配器可靠。
- 写入完成会 `Resetting...` —— 这是**冷复位**，热启动意图位被清掉，**需要长按功能键 3 秒**才进应用。

### 开发 Boot 闭环（上游强制规则）

串口烧录时，开发阶段要把 `res/arcs-mini/boot-dev-autostart.bin` 写到 `0x0`，
让设备复位后**直接进业务不用长按**；开发完成后必须把 `res/arcs-mini/boot.bin` 写回 `0x0`。

> ⚠️ `boot-dev-autostart.bin` **只有 mini（LS2684）有**，`res/arcs-mini3/` 里没有。
> **绝不**拿 mini 的 boot 顶到 mini3 上——那是另一块板（黑屏 + USB 不识别）。

## 4. 通道三：官方 GUI（.lpk 整包）

官方发布的 `.lpk` 就是一个 zip（`manifest.json` + 各分区 bin）。用官方 cskburn desktop 刷，
或用上面的 cskburn 逐分区刷。适合"回滚到官方原版"：

```powershell
# 例：恢复官方 3.0.3（app 镜像从 tag 编：git worktree add ../arcs_mini-303 mini-v3.0.3）
cskburn -C arcs -s COM10 -b 921600 --verify-all `
  0x0 res/arcs-mini3/boot.bin  0x40000 res/arcs-mini3/ap.bin  0x600000 build-303/arcs-mini.bin
adb shell version    # 应回 3.0.3-23fca91e
```

## 5. 故障 → 处置

| 现象 | 判断 | 处置 |
|---|---|---|
| `adb devices` 空但设备管理器有 `VID_0483&PID_0ADB` | ADB 接口绑在 inbox WinUSB，Google adb 找不到 | `$env:ADB_LIBUSB=1; adb start-server` |
| 烧完黑屏、USB 完全消失 | 冷复位等长按，或镜像本身不启动 | 先长按 3 秒；仍不行走 cskburn 整包回官方 |
| 反复重启 + `BOOT-*` | 卡在 recovery / boot 分区异常 | 进 recovery 重烧 app；仍循环 → 串口整包 |
| 屏幕不亮但串口能识别 flash | SoC 活着，应用没起来 | cskburn 重写 boot/ap/app |
| 同型号两台设备分不清 | 序列号相同 | 用 `transport_id` 指定 |

## 6. 烧录前必须说明的三件事

写哪个分区、哪个地址、目标序列号。多设备环境不要裸跑 `adb push`。
