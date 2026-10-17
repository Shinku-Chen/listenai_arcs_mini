# 01 快速开始：从零到设备上跑起来

目标：**编译出 `arcs_mini3` 镜像 → 烧进板子 → 抓到日志 → 看到自己的功能生效**。

## 0. 前置

| 项 | 要求 |
|---|---|
| 板子 | Arcs-mini3（LS2663）。同系列 `arcs_mini` 是另一块板，镜像不通用 |
| 系统 | Windows（PowerShell）/ Linux / WSL 均可；本仓脚本两套都有 |
| USB | 一根**能传数据**的 Type-C 线；另有 CH340 串口时更稳（可做救砖） |

## 1. 装工具链

```powershell
.\arcs-sdk\env.ps1            # 检测 + 按需下载 + 设置 ARCS_BASE / NUCLEI_TOOLCHAIN_PATH / LISTENAI_TOOLS_PATH
.\arcs-sdk\env.ps1 check      # 体检
```

```bash
source ./arcs-sdk/env.sh      # Linux/WSL 必须 source（直接执行会立刻退出）
```

macOS：`brew install listenai/tap/arcs-toolchain`（`env.sh` 不替 macOS 编 GCC）。
其它平台：`ghcr.io/listenai/arcs-builder:latest` 容器里跑 `./build.sh`。

## 2. 编译

```powershell
.\build.ps1 -S .\apps\arcs-mini -DBOARD=arcs_mini3
```

产物：`build\arcs-mini.bin`（app 分区镜像）。

> **已知坑**：`build.ps1` 在这台机器上会被 PowerShell 判成失败——CMake 把 Kconfig 的
> `warning: default on the choice symbol …` 写到 stderr，而脚本设了 `$ErrorActionPreference=Stop`。
> 配置其实是成功的。等价做法是直接调 cmake + ninja（见 [04-pitfalls.md](04-pitfalls.md)）。
> 编译失败/配置缓存脏时：`.\build.ps1 -C` 或删掉 `build*` 目录重建。

## 3. 烧录（只写 app 分区，最小改动）

```powershell
.\adb_download.ps1 -S res\arcs-mini3 -B build app    # 只烧 CP app
```

设备会自己进 recovery（`BOOT-*`），脚本等它出现、推送、重启。细节与其它模式见 [03-flashing.md](03-flashing.md)。

## 4. 抓日志

```powershell
mkdir tmp -Force | Out-Null
adb shell reboot; adb wait-for-device; timeout 10 adb shell 2>&1 > tmp/run-log.txt
```

Windows 没有 picocom，串口日志用 PowerShell 的 `System.IO.Ports.SerialPort <COMx>,921600`
（脚本示例见上游 `AGENTS.md`「日志」一节）。

## 5. 最小验收清单

| 项 | 通过标准 |
|---|---|
| 启动 | 日志里 `TAG` 的初始化行按顺序出现，无 panic / 反复重启 |
| 命令 | `adb shell version` 返回你刚编的版本号 |
| 界面 | 目标页面显示正确（中文字形完整、没有越界黑块） |
| 既有能力 | 配网、助手对话、音量、闹钟仍可用 |
| 长跑 | 10 分钟不 OOM、不自动断连、不自动复位 |

能做成 shell 命令的状态就做成命令（`SHELL_EXPORT_CMD`），比反复烧录快一个数量级。
BLE 这类协议功能可以先用主机侧对端跑通（本仓 `tools/intercom_peer.py` 是范例）。
