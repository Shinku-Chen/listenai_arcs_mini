#ifndef LISA_CAMERA_RGB565_H
#define LISA_CAMERA_RGB565_H
#include <stdint.h>
#include <stdbool.h>

/* Synchronous CPDMA nearest-neighbour reduction, optional clockwise 90 degree
 * rotation and byte swap. Source is tightly packed; destination must not overlap.
 * Destination address and output byte count must be cache-line (32-byte) aligned.
 * scaled_w/h describe the image before rotation. Returns 0 on success; callers
 * may use their CPU path when no DMA channel or memory is available. */
int lisa_camera_rgb565_transform(const uint8_t *src, uint16_t src_w, uint16_t src_h,
                                 uint8_t *dst, uint32_t capacity, uint16_t scaled_w,
                                 uint16_t scaled_h, bool clockwise, bool swap_bytes);
#endif
