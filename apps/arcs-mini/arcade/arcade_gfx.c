/*
 * 内置小游戏的共用绘制原语。见 arcade_gfx.h。
 *
 * 这里原来有两份拷贝（小恐龙一份、跳一跳一份），字模表 130 多行重复太蠢，
 * 统一挪到这里。函数都带 arcade_gfx_ 前缀，避免和游戏内部的静态函数撞名。
 */
#include <stddef.h>
#include <string.h>

#include "arcade_gfx.h"

/* ---------------- 5x7 点阵字 ---------------- */
/* 每字节一列，bit0 = 最上面一行（经典 5x7 LCD 字模） */

static const uint8_t k_font_digit[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E},   /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00},   /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46},   /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31},   /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10},   /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39},   /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30},   /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03},   /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36},   /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E},   /* 9 */
};

static const uint8_t k_font_alpha[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E},   /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36},   /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22},   /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C},   /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41},   /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01},   /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A},   /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F},   /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00},   /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01},   /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41},   /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40},   /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},   /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F},   /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E},   /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06},   /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E},   /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46},   /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31},   /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01},   /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F},   /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F},   /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F},   /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63},   /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07},   /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43},   /* Z */
};

static const uint8_t k_font_dash[5]  = {0x08, 0x08, 0x08, 0x08, 0x08};
static const uint8_t k_font_colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
static const uint8_t k_font_plus[5]  = {0x08, 0x08, 0x3E, 0x08, 0x08};
static const uint8_t k_font_dot[5]   = {0x00, 0x60, 0x60, 0x00, 0x00};
static const uint8_t k_font_slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};

static const uint8_t *font_glyph(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return k_font_digit[ch - '0'];
    }
    if (ch >= 'A' && ch <= 'Z') {
        return k_font_alpha[ch - 'A'];
    }
    if (ch >= 'a' && ch <= 'z') {
        return k_font_alpha[ch - 'a'];
    }
    switch (ch) {
    case '-':
        return k_font_dash;
    case ':':
        return k_font_colon;
    case '+':
        return k_font_plus;
    case '.':
        return k_font_dot;
    case '/':
        return k_font_slash;
    default:
        return NULL;
    }
}

/* ---------------- 绘制 ---------------- */

void arcade_gfx_fill(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > ARCADE_GFX_W) {
        w = ARCADE_GFX_W - x;
    }
    if (y + h > ARCADE_GFX_H) {
        h = ARCADE_GFX_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    uint32_t pair = ((uint32_t)color << 16) | color;

    for (int row = 0; row < h; row++) {
        uint16_t *d = fb + (size_t)(y + row) * ARCADE_GFX_W + x;
        int n = w;

        /* 32 位写要 4 字节对齐：列号为奇数时先单独写一个像素 */
        if (((uintptr_t)d & 3U) != 0U) {
            *d++ = color;
            n--;
        }

        uint32_t *p = (uint32_t *)d;
        int nw = n / 2;

        for (int i = 0; i < nw; i++) {
            p[i] = pair;
        }
        if (n & 1) {
            d[n - 1] = color;
        }
    }
}

void arcade_gfx_sprite(uint16_t *fb, int x, int y, const char *const *rows,
                       int nrows, int scale, uint16_t color)
{
    for (int r = 0; r < nrows; r++) {
        const char *line = rows[r];

        for (int c = 0; line[c] != '\0'; c++) {
            if (line[c] == '#') {
                arcade_gfx_fill(fb, x + c * scale, y + r * scale, scale, scale, color);
            }
        }
    }
}

void arcade_gfx_char(uint16_t *fb, int x, int y, char ch, int scale, uint16_t color)
{
    const uint8_t *cols = font_glyph(ch);

    if (cols == NULL) {
        return;
    }

    for (int c = 0; c < 5; c++) {
        uint8_t bits = cols[c];

        for (int r = 0; r < 7; r++) {
            if ((bits & (1U << r)) != 0U) {
                arcade_gfx_fill(fb, x + c * scale, y + r * scale, scale, scale, color);
            }
        }
    }
}

void arcade_gfx_text(uint16_t *fb, int x, int y, const char *s, int scale,
                     uint16_t color)
{
    for (; *s != '\0'; s++) {
        arcade_gfx_char(fb, x, y, *s, scale, color);
        x += 6 * scale;
    }
}

int arcade_gfx_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);

    return n > 0 ? n * 6 * scale - scale : 0;
}

void arcade_gfx_text_center(uint16_t *fb, int y, const char *s, int scale,
                            uint16_t color)
{
    arcade_gfx_text(fb, (ARCADE_GFX_W - arcade_gfx_text_width(s, scale)) / 2, y, s,
                    scale, color);
}
