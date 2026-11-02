#ifndef CONFIG_LISA_JPEG
#define TJE_IMPLEMENTATION
#include "tiny_jpeg.h"
#endif
#include "img_jpeg.h"
#include "sysheap.h"
#include "lisa_log.h"
#include <string.h>
#include <stdbool.h>
#ifdef CONFIG_LISA_JPEG
#include "lisa_jpeg.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static SemaphoreHandle_t codec_mutex;

int image_jpeg_lock(void)
{
    taskENTER_CRITICAL();
    SemaphoreHandle_t mutex = codec_mutex;
    taskEXIT_CRITICAL();
    if (!mutex) {
        SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
        if (!candidate) {
            return -1;
        }
        taskENTER_CRITICAL();
        if (!codec_mutex) {
            codec_mutex = candidate;
            candidate = NULL;
        }
        mutex = codec_mutex;
        taskEXIT_CRITICAL();
        if (candidate) {
            vSemaphoreDelete(candidate);
        }
    }
    return xSemaphoreTake(mutex, pdMS_TO_TICKS(2000)) == pdTRUE ? 0 : -1;
}

void image_jpeg_unlock(void)
{
    xSemaphoreGive(codec_mutex);
}
#endif

#define TAG "img_jpeg"

#ifndef CONFIG_LISA_JPEG
struct jpeg_write_context {
	uint8_t *buf;
	uint32_t total_size;
	uint32_t wrote;
    bool overflow;
};

static void jpeg_write(void *context, void *data, int size)
{
	struct jpeg_write_context *ctx = (struct jpeg_write_context *)context;
	if (!ctx || !data || size <= 0) {
		return;
	}

	uint32_t can_write = ctx->total_size - ctx->wrote;
    if ((uint32_t)size > can_write) {
        ctx->overflow = true;
        return;
    }
	can_write = can_write >= (uint32_t)size ? size : can_write;

	if (can_write > 0) {
		memcpy(ctx->buf + ctx->wrote, data, can_write);
		ctx->wrote += can_write;
	}
}

#endif

static void rgb565_to_rgb888(const uint16_t *rgb565, uint8_t *rgb888, uint32_t pixel_count)
{
	for (uint32_t i = 0; i < pixel_count; i++) {
		uint16_t pixel = rgb565[i];
		uint8_t r5 = (pixel >> 11) & 0x1F;
		uint8_t g6 = (pixel >> 5) & 0x3F;
		uint8_t b5 = pixel & 0x1F;

		rgb888[i * 3 + 0] = (r5 * 255 + 15) / 31;
		rgb888[i * 3 + 1] = (g6 * 255 + 31) / 63;
		rgb888[i * 3 + 2] = (b5 * 255 + 15) / 31;
	}
}

int image_rgb565_to_jpeg(const uint8_t *rgb565, uint32_t len, int width, int height,
			 uint8_t **jpeg_data, uint32_t *jpeg_len)
{
	if (!rgb565 || !jpeg_data || !jpeg_len || width <= 0 || height <= 0 ||
        width > 4096 || height > 4096 || len != (uint32_t)width * (uint32_t)height * 2U) {
		LISA_LOGE(TAG, "Invalid parameters");
		return -1;
	}

	*jpeg_data = NULL;
    *jpeg_len = 0;
	uint32_t pixel_count = len / 2;

#ifdef CONFIG_LISA_JPEG
    /* Include the JPEG header and alignment for in-place DMA output. */
    uint32_t aligned_pixels = ((width + 15U) & ~15U) * ((height + 15U) & ~15U);
    uint32_t capacity = aligned_pixels * 12U + 1088U;
    uint8_t *encoded = psram_malloc(capacity);
    uint32_t size = 0;
    if (!encoded) {
		LISA_LOGE(TAG, "Failed to allocate JPEG buffer");
		return -1;
	}

    /* The MCU extraction stage consumes the RGB888 input before the hardware
     * writes the compressed stream. Reuse the output allocation for the
     * converted input so the camera path does not need another 460800-byte
     * PSRAM block for a 320x240 frame. */
    rgb565_to_rgb888((const uint16_t *)rgb565, encoded, pixel_count);

    if (image_jpeg_lock() != 0) {
        psram_free(encoded);
        return -1;
    }
    lisa_jpeg_encoder_cfg_t cfg = {
        .width = width,
        .height = height,
        .input_format = LISA_JPEG_PIXEL_FORMAT_RGB888,
    };
    uint32_t ret = lisa_jpeg_encoder_bounded(encoded, encoded, capacity, &size, cfg);
    image_jpeg_unlock();
    if (ret != 0 || size == 0 || size > capacity) {
        psram_free(encoded);
        LISA_LOGE(TAG, "Hardware JPEG encoding failed: %u", ret);
        return -1;
    }
    /* Keep only the compressed bytes alive while the network upload runs. */
    uint8_t *compact = psram_malloc(size);
    if (compact) {
        memcpy(compact, encoded, size);
        psram_free(encoded);
        encoded = compact;
    }
    *jpeg_data = encoded;
    *jpeg_len = size;
    return 0;
#else
	uint32_t jpeg_buf_size = width * height * 3 / 5;
	if (jpeg_buf_size < 1024) {
		jpeg_buf_size = 1024;
	}

	uint8_t *jpeg_buf = psram_malloc(jpeg_buf_size);
	if (!jpeg_buf) {
		LISA_LOGE(TAG, "Failed to allocate JPEG buffer");
		return -1;
	}

	struct jpeg_write_context ctx = {
		.buf = jpeg_buf,
		.total_size = jpeg_buf_size,
		.wrote = 0,
	};

	uint8_t *rgb888 = psram_malloc(pixel_count * 3);
	if (!rgb888) {
		psram_free(jpeg_buf);
		LISA_LOGE(TAG, "Failed to allocate RGB888 buffer");
		return -1;
	}
	rgb565_to_rgb888((const uint16_t *)rgb565, rgb888, pixel_count);
	int result = tje_encode_with_func(jpeg_write, &ctx, 1, width, height, 3, rgb888);
	psram_free(rgb888);

	if (result == 0 || ctx.overflow) {
		LISA_LOGE(TAG, "JPEG encoding failed");
		psram_free(jpeg_buf);
		return -1;
	}

	*jpeg_data = jpeg_buf;
	*jpeg_len = ctx.wrote;

	return 0;
#endif
}

void image_jpeg_free(void *jpeg)
{
	if (jpeg) {
		psram_free(jpeg);
	}
}
