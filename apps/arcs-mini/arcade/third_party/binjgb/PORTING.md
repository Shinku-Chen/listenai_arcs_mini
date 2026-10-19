# binjgb（vendored）

GB / GBC 模拟器内核，移植自 <https://github.com/binji/binjgb>。

- 上游 commit：`16621111ed0ee73bcc45c912a823bcebedcffc0f`（2026-09-25，`Set sprite palette to SGB color area (#82)`）
- 许可证：MIT（见 `LICENSE`）

## 为什么选它

前一个内核 Peanut-GB 只有 DMG：`SVBK`(0xFF70) / `VBK`(0x4F) / `HDMA5`(0x55)
一处实现都没有，GBC 游戏（Pokémon Crystal 汉化版，`0x143=0x80`、`0x147=0x10`）
片头能动、一进正式流程就卡死在 pokecrystal 的 `DelayFrame()` 里。

硬指标对照：

| 指标 | binjgb |
| --- | --- |
| 语言 | 纯 C99（无 C++ 运行时依赖） |
| CGB | VRAM/WRAM 分 bank、CGB 调色板、GDMA/HDMA、双速模式、MBC1/2/3/5 + RTC |
| 许可证 | MIT |
| 运行期内存 | `Emulator` 结构体 ~295 KB（裁剪 SGB 缓冲后）+ ROM |
| 依赖 | libc 的 `memcpy`/`printf` 而已，无线程、无文件系统强依赖 |

## 只搬了 8 个文件

上游 `src/` 里还有 host（SDL/OpenGL）、rewind、tester、imgui 调试器；本固件
只要核心，所以只保留：

```
emulator.c  emulator.h  common.c  common.h  memory.c  memory.h
builtin-palettes.def  LICENSE
```

`joypad.c` / `rewind.c` / `host*.c` / `tester.c` / `emulator-debug.c` 没有搬。

## 对上游源码的改动（全部有 `ARCS-MINI port` 注释）

1. **`memory.h`**：`xmalloc` / `xcalloc` / `xrealloc` / `xfree` 改指向
   `arcade_gbc.c` 里的 `arcade_bin_*`。
   原因：默认 `malloc` 走内部 SRAM，几十 KB 就满；而模拟器一份 `Emulator`
   近 300 KB、ROM 2 MB 起，必须放 PSRAM。同时本平台 `exram_malloc()` 失败会走
   `sysheap.c` 的 `CRASH_ON_ALLOC_FAIL` → `__builtin_trap()`（整机死机、屏幕
   冻结、`adb shell` 永久挂住），所以包装层要先查余量、失败返回 `NULL`。
2. **`emulator.c` 顶部**：显式 `#define NDEBUG` 再 `#include <assert.h>`，
   关掉断言（省 flash，也避免第三方断言把整机打死）。
3. **`struct Emulator`**：`SgbFrameBuffer sgb_frame_buffer;`（内联
   256×224×4B = 229 KB）改成指针，只有真 SGB ROM 才在 `init_emulator()`
   里分配。SGB（SFC 增强 GB）游戏极少，省下的 229 KB PSRAM 留给 ROM 和存档。
   涉及 4 处：结构体字段、`init_emulator()` 按需分配、`emulator_delete()` 释放、
   `emulator_get_sgb_frame_buffer()`、`CHR_TRN` 写入时的空指针保护。
4. **`emulator_new()`**：`xcalloc()` 之后补 `CHECK(e != NULL)`，否则 PSRAM
   分配失败会直接空指针解引用。
5. **文件末尾新增两个只读访问器**（`emulator_arcade_snapshot()` /
   `emulator_arcade_read()`，声明在 `emulator.h`）：原版把 CPU 寄存器和内存
   都藏在 `.c` 内部，而固件需要在 `adb shell` 里用 `arcade cpu` /
   `arcade peek` 判断「卡在哪个 PC」和「存档区有没有写进去」。

## 和固件的接口

外壳是 `apps/arcs-mini/arcade/arcade_gbc.c`，实现 `arcade_gb.h` 的同一套接口
（`start` / `stop` / `run_frame` / `set_input` + 调试命令），由
`CONFIG_ARCADE_GB_CORE` 决定编译它还是 Peanut-GB 版 `arcade_gb.c`。

- 逐帧推进：`until = emulator_get_ticks() + PPU_FRAME_TICKS`，循环
  `emulator_run_until()` 直到出现 `UNTIL_TICKS` / 断点 / 非法指令，保证每次
  调用一定前进一帧（不会自旋）。
- 画面：`FrameBuffer`（160×144，`0xAABBGGRR`）→ `arcade_video_draw_frame_rgba()`
  1.5 倍最近邻放大到 240×216 LVGL canvas。
- 存档：`emulator_was_ext_ram_updated()` 变化时把模拟器内的 ext RAM 同步到
  `arcade_save` 的缓冲并标脏，落盘仍走原来的 `.sav` 原子写。
- 声音：本固件没有音频链路，`config.disable_sound[] = TRUE` 跳过混音；
  但 `audio_frequency` 不能是 0（`emulator_run_until()` 会拿它做除法），
  所以给一个 100 ms 的小缓冲。
