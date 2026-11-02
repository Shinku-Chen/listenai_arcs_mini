/* Validate the subset understood by the ARCS baseline JPEG hardware/parser.
 * Reject unsupported layouts before allocating output or programming DMA. */
#include "lisa_jpeg.h"
#include <stdbool.h>

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

int lisa_jpeg_get_info(const uint8_t *data, uint32_t size, uint16_t *width, uint16_t *height)
{
    uint32_t pos = 2;
    uint8_t quant = 0, huffman = 0, needed_quant = 0;
    uint16_t w = 0, h = 0;
    if (!data || !width || !height || size < 4 || data[0] != 0xff || data[1] != 0xd8) {
        return -1;
    }
    while (pos < size) {
        if (data[pos++] != 0xff) {
            return -1;
        }
        while (pos < size && data[pos] == 0xff) {
            pos++;
        }
        if (size - pos < 3) {
            return -1;
        }
        uint8_t marker = data[pos++];
        uint32_t length = read_be16(data + pos);
        if (length < 2 || length > size - pos) {
            return -1;
        }
        const uint8_t *p = data + pos + 2;
        uint32_t count = length - 2;
        pos += length;
        switch (marker) {
        case 0xc0:
            /* Limit the hardware path to baseline YCbCr with canonical IDs.
             * Grayscale/CMYK/progressive and unusual subsampling use software. */
            if (w || count != 15 || p[0] != 8 || p[5] != 3 ||
                p[6] != 1 || p[9] != 2 || p[12] != 3 ||
                (p[7] != 0x11 && p[7] != 0x21 && p[7] != 0x22) ||
                p[10] != 0x11 || p[13] != 0x11 || p[8] > 1 || p[11] > 1 || p[14] > 1) {
                return -1;
            }
            h = read_be16(p + 1);
            w = read_be16(p + 3);
            if (!w || !h || w > 4096 || h > 4096) {
                return -1;
            }
            needed_quant = (1U << p[8]) | (1U << p[11]) | (1U << p[14]);
            break;
        case 0xdb:
            if (!count || count % 65) {
                return -1;
            }
            while (count) {
                if (p[0] > 1) {
                    return -1;
                }
                quant |= 1U << p[0];
                for (unsigned i = 1; i <= 64; i++) {
                    if (!p[i]) {
                        return -1;
                    }
                }
                p += 65;
                count -= 65;
            }
            break;
        case 0xc4:
            if (!count) {
                return -1;
            }
            while (count) {
                if (count < 17 || (p[0] != 0 && p[0] != 1 && p[0] != 0x10 && p[0] != 0x11)) {
                    return -1;
                }
                uint32_t symbols = 0;
                int slots = 1;
                for (unsigned i = 1; i <= 16; i++) {
                    slots = slots * 2 - p[i];
                    symbols += p[i];
                    if (slots < 0) {
                        return -1;
                    }
                }
                bool ac = (p[0] & 0x10) != 0;
                if (!symbols || symbols > (ac ? 162U : 12U) || symbols > count - 17) {
                    return -1;
                }
                for (unsigned i = 0; i < symbols; i++) {
                    uint8_t v = p[17 + i];
                    if ((!ac && v > 11) || (ac && ((v & 15) > 10 ||
                        ((v & 15) == 0 && v != 0 && v != 0xf0)))) {
                        return -1;
                    }
                }
                huffman |= 1U << ((ac ? 2 : 0) + (p[0] & 1));
                p += 17 + symbols;
                count -= 17 + symbols;
            }
            break;
        case 0xdd:
            if (count != 2) {
                return -1;
            }
            break;
        case 0xda: {
            if (!w || (quant & needed_quant) != needed_quant || count != 10 || p[0] != 3 ||
                p[1] != 1 || p[3] != 2 || p[5] != 3 || p[7] != 0 || p[8] != 63 || p[9] != 0) {
                return -1;
            }
            for (unsigned i = 2; i <= 6; i += 2) {
                uint8_t dc = p[i] >> 4, ac = p[i] & 15;
                if (dc > 1 || ac > 1 || !(huffman & (1U << dc)) || !(huffman & (1U << (ac + 2)))) {
                    return -1;
                }
            }
            /* One complete interleaved scan, terminating in EOI. The legacy
             * parser walks entropy bytes without bounds checks. */
            while (pos < size) {
                if (data[pos++] != 0xff) {
                    continue;
                }
                if (pos == size) {
                    return -1;
                }
                uint8_t code = data[pos++];
                if (code == 0 || (code >= 0xd0 && code <= 0xd7)) {
                    continue;
                }
                if (code == 0xd9 && pos == size) {
                    *width = w;
                    *height = h;
                    return 0;
                }
                return -1;
            }
            return -1;
        }
        default:
            if ((marker < 0xe0 || marker > 0xef) && marker != 0xfe) {
                return -1;
            }
            break;
        }
    }
    return -1;
}

uint32_t lisa_jpeg_decoder_bounded(const uint8_t *data, uint32_t size, uint8_t *output,
                                   uint32_t capacity, uint16_t *width, uint16_t *height)
{
    uint16_t w, h;
    if (!output || !width || !height || lisa_jpeg_get_info(data, size, &w, &h) != 0 ||
        (uint32_t)w * h > capacity / 2U) {
        return CSK_DRIVER_ERROR;
    }
    *width = 0;
    *height = 0;
    lisa_jpeg_decoder_cfg_t cfg = {.output_format = LISA_JPEG_PIXEL_FORMAT_RGB565};
    return lisa_jpeg_decoder(data, size, output, width, height, cfg);
}
