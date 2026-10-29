/*
 * 网易云音乐应用层（里程碑 2）：页面状态、按键语义、歌单/歌曲加载、封面、
 * 歌词、扫码登录。
 *
 * 分工沿用 radio 的做法：
 *  - music_app 持有全部状态（PSRAM），界面每 100 ms 通过 music_app_get_model()
 *    取一份快照渲染，所以渲染侧不需要任何跨线程回调；
 *  - 网络 / 解码一律在 music.app 工作线程里跑（letter-shell 任务栈只有 1 KB，
 *    LVGL 线程更不能阻塞），shell 命令只提交请求 + 等完成信号。
 *
 * 页面（都是同一个 nav screen 里的子页，用 HIDDEN 切换）：
 *   PLAYER   播放页：封面 / 歌名 / 歌手 / 进度 / 当前歌词 / 提示
 *   MENU     导航页：3 个推荐歌单 + 歌词 + 扫码登录
 *   SONGLIST 歌曲列表（最多 10 首，两行式）
 *   LYRIC    歌词页（当前行 + 上下各两行）
 *   LOGIN    扫码登录（二维码 + 轮询状态）
 *
 * 按键（单键，与电量键系统语义保持一致）：
 *   单击 PLAYER=下一首 / MENU=下移 / SONGLIST=下移 / LYRIC=下一首 / LOGIN=刷新二维码
 *   双击 PLAYER=播放暂停 / MENU=进入 / SONGLIST=播放选中 / LYRIC=播放暂停 / LOGIN=立即查询
 *   三击 PLAYER=进导航页 / 子页=回上一级（SONGLIST→MENU，其余→PLAYER）
 *   四击 同三击
 *   长按 关机（交给 radio 的系统语义处理，music 不消费）
 *   5/6/7 击 交给 radio 进 BLE 配网页
 */
#ifndef __MUSIC_APP_H__
#define __MUSIC_APP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSIC_UI_ROWS        5
#define MUSIC_TITLE_MAX      72
#define MUSIC_SUB_MAX        48
#define MUSIC_LYRIC_LINE     96
#define MUSIC_QR_URL_MAX     96

typedef enum {
    MUSIC_PAGE_PLAYER = 0,
    MUSIC_PAGE_MENU,
    MUSIC_PAGE_SONGLIST,
    MUSIC_PAGE_LYRIC,
    MUSIC_PAGE_LOGIN,
} music_page_t;

/* 界面每帧读取的快照 */
typedef struct {
    uint8_t page;        /* music_page_t */
    uint8_t menu_index;  /* 导航页选中项 */
    uint8_t list_index;  /* 歌曲列表选中项 */
    uint8_t list_window; /* 歌曲列表窗口起点 */
    uint8_t net_ok;
    uint8_t busy;        /* 正在联网加载 */
    uint8_t playing;     /* 1 = 正在出声 */
    uint8_t paused;      /* 用户手动暂停 */
    uint8_t volume;      /* 0-100 */
    uint8_t song_index;  /* 1-based，0 表示还没选曲 */
    uint8_t song_count;
    uint16_t qr_state;   /* 0 未开始 / 800 过期 / 801 待扫码 / 802 待确认 / 803 成功 */
    uint8_t logged_in;   /* 1 = 已保存登录 cookie */
    uint8_t lyric_valid;
    int8_t lyric_center; /* 当前行在窗口里的位置（0..MUSIC_UI_ROWS-1） */
    uint8_t row_count;   /* 歌曲列表本次窗口里有效的行数 */
    uint32_t pos_ms;
    uint32_t dur_ms;
    uint32_t art_gen;    /* 封面版本号：变了就去 music_app_cover_copy() */
    uint32_t art_len;
    uint16_t art_w;      /* 封面 JPEG 的实际宽高（RAW 解码必须对得上） */
    uint16_t art_h;
    uint32_t qr_gen;     /* 二维码版本号：变了就重画 */
    char title[MUSIC_TITLE_MAX];
    char artist[MUSIC_SUB_MAX];
    char album[MUSIC_SUB_MAX];
    char detail[MUSIC_SUB_MAX]; /* 非空时用红字显示（出错/提示） */
    char list_title[MUSIC_TITLE_MAX];
    char menu_name[MUSIC_UI_ROWS][20];
    char menu_value[MUSIC_UI_ROWS][20];
    char row_title[MUSIC_UI_ROWS][MUSIC_TITLE_MAX];
    char row_sub[MUSIC_UI_ROWS][MUSIC_SUB_MAX];
    char lyric[MUSIC_UI_ROWS][MUSIC_LYRIC_LINE];
    char qr_url[MUSIC_QR_URL_MAX];
    char logon[20];
    char qr_hint[MUSIC_SUB_MAX];
} music_ui_model_t;

/* 初始化：注册 nav screen、恢复 KV、起工作线程。必须在 LVGL 就绪后调用。 */
int music_app_init(void);

/* 按键拦截：返回 true 表示这次动作被音乐页消费掉（此时音乐页必须在栈顶） */
bool music_app_handle_action(int action);

/* 音乐页是否在导航栈顶（渲染侧/按键分发都靠它判断） */
bool music_app_is_on_top(void);

/* 把音乐页投递到 UI 线程置顶（开机取页 / 从配网页回来时用） */
void music_app_show(void);

/* 回到导航默认页（radio）：配网等系统动作需要把音乐页让开时调用 */
void music_app_hide(void);

/* 渲染侧取数；返回 false 表示取不到（持锁超时），调用方跳过本帧 */
bool music_app_get_model(music_ui_model_t *out);

/* 取封面 JPEG 原件（拷进调用方缓冲，避免和网络线程抢同一块内存） */
bool music_app_cover_copy(uint8_t *dst, size_t capacity, size_t *length);

#endif /* __MUSIC_APP_H__ */
