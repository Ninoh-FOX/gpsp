/* Koriki: borders drawn by code around the game picture, for any surface size
 * and any picture rectangle (each scaler leaves a different free area). */
#ifndef KORIKI_FRAME_H
#define KORIKI_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	KF_BORDER_NONE = 0,
	KF_BORDER_DMG,      /* grey case, DOT MATRIX band, BATTERY lamp */
	KF_BORDER_POCKET,   /* dark case, POWER lamp, name bottom left */
	KF_BORDER_COLOR,    /* black case, POWER lamp, name bottom centre */
	KF_BORDER_GBA,      /* black, POWER lamp, GAME BOY ADVANCE bottom centre */
	KF_BORDER_SP        /* black, GAME BOY ADVANCE SP bottom centre */
};

/* "DMG", "POCKET", "COLOR", "GBA", "SP" -> style; anything else -> KF_BORDER_NONE */
int kf_border_from_name(const char *name);

/* Paints the whole RGB565 surface (pitch in pixels) with the border of
 * the style around the picture rectangle (wx, wy, ww, wh). The rectangle is
 * painted too (black): the picture is drawn over it afterwards. */
void kf_draw_border(uint16_t *px, int pitch, int w, int h,
                    int wx, int wy, int ww, int wh, int style);

/* Picture (sw x sh) scaled to dw x dh (nearest pixel) with a grid: the last
 * column and row of the screen pixels of every emulated pixel are mixed with
 * the grid colour (RGB565), so the grid always falls between the emulated
 * pixels. With a whole scale (2x, 3x) every block is the same size; with
 * another (8/3, 10/3...) the blocks alternate (3,3,2...). dw <= 1024,
 * dh <= 1024. strength: 0 = no grid (nearest pixel only), 1 = soft,
 * 2 = strong. Pitches in pixels. */
void kf_scale_grid(uint16_t *dst, int dpitch, const uint16_t *src, int spitch,
                   int sw, int sh, int dw, int dh, uint16_t grid, int strength);

/* Grid laid over a picture already on the screen (w x h, pitch in pixels),
 * the same everywhere: for the stretched scalings, where the pixels are not
 * all the same size. strength: 1 = soft, 2 = strong (as the ATC overlay). */
void kf_overlay_grid(uint16_t *px, int pitch, int w, int h, int strength);

#ifdef __cplusplus
}
#endif

#endif
