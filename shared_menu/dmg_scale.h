#ifndef DMG_SCALE_H
#define DMG_SCALE_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 1D scaling weight entry for Sharp-Shimmerless (area-averaging / box-filter interpolation).
 */
typedef struct {
    uint16_t src_idx;  /* Primary source pixel / line index */
    uint8_t  weight1;  /* Blend weight for (src_idx + 1) in 0..255. 0 = 100% src_idx */
} dmg_scale_weight_t;

/**
 * Computes 1D box-filter area interpolation weights for scaling src_len to dst_len.
 *
 * If dst_len or src_len is 0, does nothing.
 * For each destination pixel x:
 * - If the pixel falls completely inside source pixel t0, weight1 is 0 (pure source pixel).
 * - If the pixel spans the boundary between t0 and t1, weight1 is proportional to the
 *   fractional overlap of t1.
 */
static inline void dmg_init_shimmerless_weights(uint16_t src_len, uint16_t dst_len, dmg_scale_weight_t *weights)
{
    if (dst_len == 0 || src_len == 0 || weights == NULL) return;

    float inv_scale = (float)src_len / (float)dst_len;
    float scale = (float)dst_len / (float)src_len;

    for (uint16_t i = 0; i < dst_len; ++i) {
        float u0 = (float)i * inv_scale;
        float u1 = (float)(i + 1) * inv_scale;
        int t0 = (int)floorf(u0);
        int t1 = (int)floorf(u1);

        if (t0 >= (int)src_len) t0 = (int)src_len - 1;
        if (t1 >= (int)src_len) t1 = (int)src_len - 1;

        weights[i].src_idx = (uint16_t)t0;
        if (t0 == t1 || t0 >= (int)src_len - 1) {
            weights[i].weight1 = 0u;
        } else {
            float frac1 = ((float)(i + 1)) - ((float)t1 * scale);
            if (frac1 < 0.0f) frac1 = 0.0f;
            if (frac1 > 1.0f) frac1 = 1.0f;
            weights[i].weight1 = (uint8_t)(frac1 * 255.0f + 0.5f);
        }
    }
}

/**
 * Fast RGB565 linear blend: blends c0 and c1 with weight w (0..255).
 * w = 0 returns c0; w = 255 returns c1.
 * Blends R and B simultaneously in one 32-bit operation, and G in a second operation.
 */
static inline uint16_t dmg_blend_rgb565(uint16_t c0, uint16_t c1, uint8_t w)
{
    if (w == 0u || c0 == c1) return c0;
    if (w == 255u) return c1;

    uint32_t w5 = (uint32_t)((w + 4u) >> 3);
    uint32_t inv_w5 = 32u - w5;

    uint32_t rb = (((c0 & 0xF81Fu) * inv_w5 + (c1 & 0xF81Fu) * w5) >> 5) & 0xF81Fu;
    uint32_t g  = (((c0 & 0x07E0u) * inv_w5 + (c1 & 0x07E0u) * w5) >> 5) & 0x07E0u;

    return (uint16_t)(rb | g);
}

/**
 * Fast RGB555 linear blend: blends c0 and c1 with weight w (0..255).
 * w = 0 returns c0; w = 255 returns c1.
 */
static inline uint16_t dmg_blend_rgb555(uint16_t c0, uint16_t c1, uint8_t w)
{
    if (w == 0u || c0 == c1) return c0;
    if (w == 255u) return c1;

    uint32_t w5 = (uint32_t)((w + 4u) >> 3);
    uint32_t inv_w5 = 32u - w5;

    uint32_t rb = (((c0 & 0x7C1Fu) * inv_w5 + (c1 & 0x7C1Fu) * w5) >> 5) & 0x7C1Fu;
    uint32_t g  = (((c0 & 0x03E0u) * inv_w5 + (c1 & 0x03E0u) * w5) >> 5) & 0x03E0u;

    return (uint16_t)(rb | g);
}

#ifdef __cplusplus
}
#endif

#endif /* DMG_SCALE_H */

