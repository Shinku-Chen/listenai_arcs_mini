#include "lisa_camera_rgb565.h"
#include "dma.h"
#include "cache.h"
#include "sysheap.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

typedef struct {
    SemaphoreHandle_t done;
    volatile uint32_t event;
} transform_wait_t;

static void transform_done(uint32_t event, uint32_t bytes, uint32_t user)
{
    (void)bytes;
    transform_wait_t *wait = (transform_wait_t *)user;
    if (event & (DMA_EVENT_TRANSFER_COMPLETE | DMA_EVENT_ERROR)) {
        BaseType_t wake = pdFALSE;
        wait->event = event;
        xSemaphoreGiveFromISR(wait->done, &wake);
        portYIELD_FROM_ISR(wake);
    }
}

int lisa_camera_rgb565_transform(const uint8_t *src, uint16_t src_w, uint16_t src_h,
                                 uint8_t *dst, uint32_t capacity, uint16_t scaled_w,
                                 uint16_t scaled_h, bool clockwise, bool swap_bytes)
{
    if (!src || !dst || !src_w || !src_h || !scaled_w || !scaled_h ||
        src_w > 4096 || src_h > 4096 || scaled_w > src_w || scaled_h > src_h ||
        src_w % scaled_w || src_h % scaled_h || (uint32_t)scaled_w * scaled_h > capacity / 2U) {
        return -1;
    }
    uint32_t src_bytes = (uint32_t)src_w * src_h * 2U;
    uint32_t dst_bytes = (uint32_t)scaled_w * scaled_h * 2U;
    if ((uintptr_t)src < (uintptr_t)dst + dst_bytes && (uintptr_t)dst < (uintptr_t)src + src_bytes) {
        return -1;
    }
    /* Invalidation must not discard neighbouring allocations' dirty bytes. */
    if (((uintptr_t)dst & 31U) || (dst_bytes & 31U)) {
        return -1;
    }
    uint32_t list_bytes = (uint32_t)scaled_h * 2U * sizeof(DMA_LLI);
    DMA_LLI *list = exram_malloc(32, list_bytes);
    transform_wait_t wait = {.done = xSemaphoreCreateBinary(), .event = 0};
    if (!list || !wait.done) {
        exram_free(list);
        if (wait.done) {
            vSemaphoreDelete(wait.done);
        }
        return -1;
    }
    uint32_t control = DMA_CH_CTLL_INT_EN | DMA_CH_CTLL_SRC_WIDTH(DMA_WIDTH_BYTE) |
                       DMA_CH_CTLL_DST_WIDTH(DMA_WIDTH_BYTE) | DMA_CH_CTLL_SRC_INC |
                       DMA_CH_CTLL_DST_INC | DMA_CH_CTLL_SRC_BSIZE(DMA_BSIZE_1) |
                       DMA_CH_CTLL_DST_BSIZE(DMA_BSIZE_1) | DMA_CH_CTLL_TTFC_M2M |
                       DMA_CH_CTLL_S_GATH_EN | DMA_CH_CTLL_D_SCAT_EN;
    uint32_t x_step = src_w / scaled_w;
    uint32_t y_step = src_h / scaled_h;
    /* Two byte lanes per input row implement RGB565 byte swapping without
     * CPU pixel loops or an intermediate image. Scatter transposes the rows. */
    for (uint32_t y = 0; y < scaled_h; y++) {
        for (uint32_t lane = 0; lane < 2; lane++) {
            uint32_t i = y * 2U + lane;
            uint32_t offset = clockwise ? (scaled_h - 1U - y) * 2U : y * scaled_w * 2U;
            list[i] = (DMA_LLI){
                .SAR = (uint32_t)(src + y * y_step * src_w * 2U + (swap_bytes ? 1U - lane : lane)),
                .DAR = (uint32_t)(dst + offset + lane),
                .LLP = i + 1U < scaled_h * 2U ? (uint32_t)&list[i + 1U] : 0,
                .CTL_LO = control | (i + 1U < scaled_h * 2U ? DMA_CH_CTLL_LLP_EN_MASK : 0),
                .u.SIZE = scaled_w,
            };
        }
    }
    taskENTER_CRITICAL();
    dma_initialize();
    taskEXIT_CRITICAL();
    /* Channel ownership is shared across cores; only use the configured
     * channel and never probe channels owned by the other core. */
    uint8_t channel = dma_channel_reserve(CONFIG_LISA_CAMERA_CPDMA_CH, transform_done,
                                          (uint32_t)&wait, DMA_CACHE_SYNC_NOP);
    int ret = -1;
    if (channel == DMA_CHANNEL_ANY) {
        goto done;
    }
    uint32_t gather = (x_step * 2U - 1U) | (1U << SG_COUNT_POS);
    uint32_t scatter = (clockwise ? scaled_h * 2U - 1U : 1U) | (1U << SG_COUNT_POS);
    ret = dma_channel_setup(channel, DMA_CH_EN_XFER_INT, control, DMA_CH_CFGL_CH_PRIOR(0),
                            DMA_CH_CFGH_FIFO_MODE, gather, scatter);
    if (ret == 0) {
        HAL_FlushDCache_by_Addr((uint32_t *)src, src_bytes);
        HAL_FlushDCache_by_Addr((uint32_t *)dst, dst_bytes);
        HAL_FlushDCache_by_Addr((uint32_t *)list, list_bytes);
        ret = dma_channel_start_LLP(channel, list);
    }
    if (ret == 0) {
        if (xSemaphoreTake(wait.done, pdMS_TO_TICKS(100)) != pdTRUE ||
            !(wait.event & DMA_EVENT_TRANSFER_COMPLETE) || (wait.event & DMA_EVENT_ERROR)) {
            ret = -1;
        }
    }
    /* Stop DMA before freeing descriptors or allowing CPU fallback to write. */
    dma_channel_disable(channel, 1);
    HAL_InvalidateDCache_by_Addr((uint32_t *)dst, dst_bytes);
    dma_channel_unreserve(channel);
done:
    taskENTER_CRITICAL();
    dma_uninitialize();
    taskEXIT_CRITICAL();
    exram_free(list);
    vSemaphoreDelete(wait.done);
    return ret;
}
