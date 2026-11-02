#include "lv_img_hw_jpeg.h"
#include "lvgl.h"
#include "lisa_jpeg.h"
#include "img_jpeg.h"
#include "sysheap.h"

/* Keep decode working sets bounded on 8 MB boards. Larger/unsupported images
 * remain the responsibility of LVGL's existing software decoder. */
#define JPEG_MAX_BYTES (512U * 1024U)
#define JPEG_MAX_PIXELS (640U * 480U)

typedef struct {
    const uint8_t *data;
    uint8_t *owned;
    uint32_t size;
} jpeg_source_t;

static int read_source(const void *src, jpeg_source_t *source)
{
    lv_img_src_t type = lv_img_src_get_type(src);
    if (type == LV_IMG_SRC_VARIABLE) {
        const lv_img_dsc_t *image = src;
        if (image->header.cf != LV_IMG_CF_RAW || !image->data || image->data_size > JPEG_MAX_BYTES) {
            return -1;
        }
        source->data = image->data;
        source->size = image->data_size;
        return 0;
    }
    if (type != LV_IMG_SRC_FILE) {
        return -1;
    }
    lv_fs_file_t file;
    if (lv_fs_open(&file, src, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        return -1;
    }
    int ret = -1;
    uint8_t magic[2];
    uint32_t count = 0;
    if (lv_fs_read(&file, magic, sizeof(magic), &count) != LV_FS_RES_OK || count != 2 ||
        magic[0] != 0xff || magic[1] != 0xd8 ||
        lv_fs_seek(&file, 0, LV_FS_SEEK_END) != LV_FS_RES_OK ||
        lv_fs_tell(&file, &source->size) != LV_FS_RES_OK || !source->size ||
        source->size > JPEG_MAX_BYTES || lv_fs_seek(&file, 0, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
        goto done;
    }
    source->owned = psram_malloc(source->size);
    if (!source->owned) {
        goto done;
    }
    count = 0;
    if (lv_fs_read(&file, source->owned, source->size, &count) != LV_FS_RES_OK || count != source->size) {
        psram_free(source->owned);
        source->owned = NULL;
        goto done;
    }
    source->data = source->owned;
    ret = 0;
done:
    lv_fs_close(&file);
    return ret;
}

static lv_res_t jpeg_info(lv_img_decoder_t *decoder, const void *src, lv_img_header_t *header)
{
    (void)decoder;
    jpeg_source_t source = {0};
    uint16_t w = 0, h = 0;
    if (read_source(src, &source) != 0) {
        return LV_RES_INV;
    }
    int ret = lisa_jpeg_get_info(source.data, source.size, &w, &h);
    psram_free(source.owned);
    if (ret != 0 || (uint32_t)w * h > JPEG_MAX_PIXELS) {
        return LV_RES_INV;
    }
    header->always_zero = 0;
    header->w = w;
    header->h = h;
    header->cf = LV_IMG_CF_TRUE_COLOR;
    return LV_RES_OK;
}

static lv_res_t jpeg_open(lv_img_decoder_t *decoder, lv_img_decoder_dsc_t *dsc)
{
    (void)decoder;
    jpeg_source_t source = {0};
    if (read_source(dsc->src, &source) != 0) {
        return LV_RES_INV;
    }
    uint32_t capacity = (uint32_t)dsc->header.w * dsc->header.h * sizeof(lv_color_t);
    uint8_t *pixels = psram_malloc(capacity);
    uint16_t w = 0, h = 0;
    uint32_t ret = 1;
    if (pixels && image_jpeg_lock() == 0) {
        ret = lisa_jpeg_decoder_bounded(source.data, source.size, pixels, capacity, &w, &h);
        image_jpeg_unlock();
    }
    psram_free(source.owned);
    if (ret != 0 || w != dsc->header.w || h != dsc->header.h) {
        psram_free(pixels);
        return LV_RES_INV;
    }
#if LV_COLOR_16_SWAP
    for (uint32_t i = 0; i < capacity; i += 2) {
        uint8_t tmp = pixels[i];
        pixels[i] = pixels[i + 1];
        pixels[i + 1] = tmp;
    }
#endif
    dsc->img_data = pixels;
    dsc->user_data = pixels;
    return LV_RES_OK;
}

static void jpeg_close(lv_img_decoder_t *decoder, lv_img_decoder_dsc_t *dsc)
{
    (void)decoder;
    psram_free(dsc->user_data);
    dsc->user_data = NULL;
    dsc->img_data = NULL;
}

void lv_img_hw_jpeg_init(void)
{
#if LV_COLOR_DEPTH == 16
    /* Registered after lv_init(): first choice, with software fallback. */
    lv_img_decoder_t *decoder = lv_img_decoder_create();
    if (decoder) {
        lv_img_decoder_set_info_cb(decoder, jpeg_info);
        lv_img_decoder_set_open_cb(decoder, jpeg_open);
        lv_img_decoder_set_close_cb(decoder, jpeg_close);
    }
#endif
}
