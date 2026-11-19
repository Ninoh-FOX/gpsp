/* Koriki: borders drawn by code (see koriki_frame.h). Everything is laid out
 * from the free area around the picture, so the same code serves 320x240 and
 * 640x480 surfaces and every scaler: a part that does not fit is left out. */
#include <math.h>
#include <string.h>
#include "koriki_frame.h"
#include "koriki_frame_font.h"
#include "koriki_frame_logos.h"

typedef struct { uint16_t *px; int pitch, w, h; } kf_surf;

static uint16_t rgb565(int r, int g, int b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* blends colour (r,g,b) with alpha a (0..255) over the pixel */
static void blend(kf_surf *s, int x, int y, int r, int g, int b, int a)
{
	uint16_t *p, d;
	int dr, dg, db;
	if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0)
		return;
	p = s->px + y * s->pitch + x;
	if (a >= 255) {
		*p = rgb565(r, g, b);
		return;
	}
	d = *p;
	dr = (d >> 11) << 3; dg = ((d >> 5) & 63) << 2; db = (d & 31) << 3;
	*p = rgb565(dr + (r - dr) * a / 255, dg + (g - dg) * a / 255, db + (b - db) * a / 255);
}

static void fill(kf_surf *s, int x, int y, int w, int h, int r, int g, int b)
{
	uint16_t c = rgb565(r, g, b);
	int i, j;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > s->w) w = s->w - x;
	if (y + h > s->h) h = s->h - y;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			s->px[(y + j) * s->pitch + x + i] = c;
}

/* ---- text ---- */

static int glyph_index(char c)
{
	const char *p = strchr(KF_FONT_CHARS, c);
	return (p && c) ? (int)(p - KF_FONT_CHARS) : 0;   /* unknown -> space */
}

static int text_width(const kf_font *f, const char *t)
{
	int w = 0;
	for (; *t; t++)
		w += f->g[glyph_index(*t)].adv;
	return w;
}

/* draws t with its top left corner at (x, y); returns the x after it */
static int text(kf_surf *s, const kf_font *f, int x, int y, const char *t, int r, int g, int b)
{
	for (; *t; t++) {
		const kf_glyph *gl = &f->g[glyph_index(*t)];
		const uint8_t *a = f->a + gl->off;
		int i, j;
		for (j = 0; j < f->h; j++)
			for (i = 0; i < gl->w; i++)
				blend(s, x + gl->xoff + i, y + j, r, g, b, a[j * gl->w + i]);
		x += gl->adv;
	}
	return x;
}

/* name of Koriki's overlays (grey on black): its grey levels are the alpha
 * of a white drawing, centred on x, top at y */
static void logo(kf_surf *s, const kf_logo *l, int cx, int y)
{
	int i, j, x = cx - l->w / 2;
	for (j = 0; j < l->h; j++)
		for (i = 0; i < l->w; i++)
			blend(s, x + i, y + j, 255, 255, 255, l->a[j * l->w + i]);
}

/* ---- shapes ---- */

/* antialiased disc */
static void disc(kf_surf *s, double cx, double cy, double rad, int r, int g, int b, int alpha)
{
	int x, y;
	for (y = (int)(cy - rad - 1); y <= (int)(cy + rad + 1); y++)
		for (x = (int)(cx - rad - 1); x <= (int)(cx + rad + 1); x++) {
			double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
			double cov = rad + 0.5 - d;
			if (cov > 0)
				blend(s, x, y, r, g, b, (int)(alpha * (cov > 1 ? 1 : cov)));
		}
}

/* arc of a ring right of (cx, cy): radius rad, thickness th, +-deg degrees */
static void arc(kf_surf *s, double cx, double cy, double rad, double th, double deg, int r, int g, int b)
{
	int x, y;
	double lim = tan(deg * M_PI / 180.0);
	for (y = (int)(cy - rad - 1); y <= (int)(cy + rad + 1); y++)
		for (x = (int)cx; x <= (int)(cx + rad + 1); x++) {
			double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
			double d = sqrt(dx * dx + dy * dy), cov;
			if (dx <= 0 || fabs(dy) > dx * lim)
				continue;
			cov = th / 2 + 0.5 - fabs(d - rad);
			if (cov > 0)
				blend(s, x, y, r, g, b, (int)(255 * (cov > 1 ? 1 : cov)));
		}
}

/* red lamp with a dark rim and a highlight */
static void lamp(kf_surf *s, double cx, double cy, double rad)
{
	disc(s, cx, cy, rad + rad * 0.25, 20, 0, 0, 200);
	disc(s, cx, cy, rad, 255, 0, 0, 255);
	disc(s, cx - rad * 0.3, cy - rad * 0.3, rad * 0.35, 255, 150, 150, 200);
}

/* shadow of the case over the picture edge, outside the rectangle */
static void window_shadow(kf_surf *s, int wx, int wy, int ww, int wh, int n)
{
	static const int alpha3[3] = { 111, 80, 53 };   /* nearest first */
	int d, i;
	for (d = 1; d <= n; d++) {
		int a = alpha3[(d - 1) * 3 / n];
		for (i = wx - d; i < wx + ww + d; i++) {
			blend(s, i, wy - d, 0, 0, 0, a);
			blend(s, i, wy + wh + d - 1, 0, 0, 0, a);
		}
		for (i = wy - d + 1; i < wy + wh + d - 1; i++) {
			blend(s, wx - d, i, 0, 0, 0, a);
			blend(s, wx + ww + d - 1, i, 0, 0, 0, a);
		}
	}
}

/* lamp in the left margin, at fraction fy of the picture height, with its
 * label under it and, when waves is set, three POWER waves to its right */
static void side_lamp(kf_surf *s, const kf_font *f, int wx, int wy, int wh, double fy,
                      double u, const char *label, int waves, int lr, int lg, int lb)
{
	double rad = 6 * u, cx = wx / 2.0, cy = wy + wh * fy;
	int lw = text_width(f, label);
	if (waves)
		cx -= 7 * u;
	if (wx < 2 * rad + 6 * u)
		return;                             /* no room for the lamp at all */
	lamp(s, cx, cy, rad);
	if (waves && cx + rad + 18 * u < wx) {
		int i;
		for (i = 0; i < 3; i++)
			arc(s, cx, cy, rad + (5 + 5 * i) * u, 1.6 * u, 40, 148, 149, 153);
	}
	/* under the lamp, and under the waves when there are any */
	if (lw + 2 <= wx)
		text(s, f, (wx - lw) / 2, (int)(cy + (waves ? rad + 11 * u : rad + 4 * u)), label, lr, lg, lb);
}

/* ---- styles ---- */

int kf_border_from_name(const char *name)
{
	if (!name) return KF_BORDER_NONE;
	if (!strcmp(name, "DMG")) return KF_BORDER_DMG;
	if (!strcmp(name, "POCKET")) return KF_BORDER_POCKET;
	if (!strcmp(name, "COLOR")) return KF_BORDER_COLOR;
	if (!strcmp(name, "GBA")) return KF_BORDER_GBA;
	if (!strcmp(name, "SP")) return KF_BORDER_SP;
	return KF_BORDER_NONE;
}

void kf_draw_border(uint16_t *px, int pitch, int w, int h,
                    int wx, int wy, int ww, int wh, int style)
{
	kf_surf s = { px, pitch, w, h };
	double u = h / 480.0;                    /* layout unit: 1 on 640x480 */
	const kf_font *lab = u < 0.75 ? &kf_font_tiny : &kf_font_small;
	const kf_font *big = u < 0.75 ? &kf_font_small : &kf_font_large;
	int shadow = u < 0.75 ? 2 : 3;
	int top = wy, bottom = h - (wy + wh);

	switch (style) {
	case KF_BORDER_DMG: {
		const char *band = "DOT MATRIX WITH STEREO SOUND";
		fill(&s, 0, 0, w, h, 81, 81, 95);
		if (top >= lab->h + 2) {
			int tw = text_width(lab, band), tx = (w - tw) / 2;
			int ty = (top - lab->h) / 2, gap = (int)(6 * u + 1);
			int lt = u < 0.75 ? 1 : 2;      /* line thickness */
			int x0 = (int)(11 * u), x1 = w - (int)(11 * u);
			int ly = ty + lab->h / 2;
			fill(&s, x0, ly - 2 * lt, tx - gap - x0, lt, 116, 13, 80);
			fill(&s, x0, ly + lt, tx - gap - x0, lt, 15, 15, 138);
			fill(&s, tx + tw + gap, ly - 2 * lt, x1 - (tx + tw + gap), lt, 116, 13, 80);
			fill(&s, tx + tw + gap, ly + lt, x1 - (tx + tw + gap), lt, 15, 15, 138);
			text(&s, lab, tx, ty, band, 173, 173, 174);
		}
		side_lamp(&s, lab, wx, wy, wh, 0.40, u, "BATTERY", 0, 173, 173, 174);
		break;
	}
	case KF_BORDER_POCKET:
		fill(&s, 0, 0, w, h, 28, 28, 28);
		side_lamp(&s, lab, wx, wy, wh, 0.24, u, "POWER", 1, 148, 149, 153);
		if (bottom >= big->h + 2)
			text(&s, big, wx, wy + wh + (bottom - big->h) / 2, "GAME BOY POCKET", 148, 149, 153);
		break;
	case KF_BORDER_COLOR: {
		/* letters of COLOR in five colours */
		static const int col[5][3] = {
			{ 230, 0, 60 }, { 0, 170, 80 }, { 255, 200, 0 }, { 0, 110, 220 }, { 160, 40, 210 } };
		fill(&s, 0, 0, w, h, 0, 0, 0);
		side_lamp(&s, lab, wx, wy, wh, 0.33, u, "POWER", 1, 148, 149, 153);
		if (bottom >= big->h + 2) {
			int tw = text_width(big, "GAME BOY ") + text_width(big, "COLOR");
			int x = (w - tw) / 2, y = wy + wh + (bottom - big->h) / 2, i;
			x = text(&s, big, x, y, "GAME BOY ", 244, 244, 244);
			for (i = 0; i < 5; i++) {
				char c[2] = { "COLOR"[i], 0 };
				x = text(&s, big, x, y, c, col[i][0], col[i][1], col[i][2]);
			}
		}
		break;
	}
	case KF_BORDER_GBA:
	case KF_BORDER_SP: {
		/* the name as it is in Koriki's GBA / GBA SP overlays */
		const kf_logo *l = style == KF_BORDER_SP ? &kf_logo_sp : &kf_logo_gba;
		fill(&s, 0, 0, w, h, 0, 0, 0);
		if (style == KF_BORDER_GBA)
			side_lamp(&s, lab, wx, wy, wh, 0.33, u, "POWER", 0, 148, 149, 153);
		if (bottom >= l->h + 2)
			logo(&s, l, w / 2, wy + wh + (bottom - l->h) / 2);
		break;
	}
	default:
		fill(&s, 0, 0, w, h, 0, 0, 0);
		return;
	}
	fill(&s, wx, wy, ww, wh, 0, 0, 0);
	window_shadow(&s, wx, wy, ww, wh, shadow);
}

/* ---- grid ---- */

/* a mixed with b, k/32 of b */
static uint16_t mix565(uint16_t a, uint16_t b, int k)
{
	int r = (a >> 11) + (((b >> 11) - (a >> 11)) * k >> 5);
	int g = ((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * k >> 5);
	int bl = (a & 31) + (((b & 31) - (a & 31)) * k >> 5);
	return (uint16_t)((r << 11) | (g << 5) | bl);
}

void kf_scale_grid(uint16_t *dst, int dpitch, const uint16_t *src, int spitch,
                   int sw, int sh, int dw, int dh, uint16_t grid, int strength)
{
	/* edge / corner share of the grid colour, in 32ths: strong = the DMS
	 * dot matrix (40 % / 60 %), soft = about half of it, 0 = no grid (just
	 * the nearest pixel) */
	int ke = strength >= 2 ? 13 : strength == 1 ? 7 : 0;
	int kc = strength >= 2 ? 19 : strength == 1 ? 11 : 0;
	/* screen columns / rows of every emulated column / row */
	static uint16_t colw[1024], rowh[1024];
	static uint16_t line_c[1024], line_e[1024];
	int x, y, i, sx, sy, dy;

	if (dw > 1024 || dh > 1024 || sw > 1024 || sh > 1024 || sw <= 0 || sh <= 0)
		return;
	for (sx = 0; sx < sw; sx++)
		colw[sx] = (uint16_t)((sx + 1) * dw / sw - sx * dw / sw);
	for (sy = 0; sy < sh; sy++)
		rowh[sy] = (uint16_t)((sy + 1) * dh / sh - sy * dh / sh);

	for (sy = 0, dy = 0; sy < sh; sy++) {
		const uint16_t *s = src + sy * spitch;
		int rows = rowh[sy];
		if (!rows)
			continue;
		/* line_c: the rows inside the block, line_e: its last row */
		for (sx = 0, x = 0; sx < sw; sx++) {
			uint16_t c = s[sx], e = mix565(c, grid, ke), k = mix565(c, grid, kc);
			int cols = colw[sx];
			if (!cols)
				continue;
			for (i = 0; i < cols - 1; i++) {
				line_c[x + i] = c;
				line_e[x + i] = e;
			}
			line_c[x + cols - 1] = e;
			line_e[x + cols - 1] = k;
			x += cols;
		}
		for (y = 0; y < rows - 1; y++)
			memcpy(dst + (dy + y) * dpitch, line_c, dw * sizeof(uint16_t));
		memcpy(dst + (dy + rows - 1) * dpitch, line_e, dw * sizeof(uint16_t));
		dy += rows;
	}
}

/* ---- grid laid over the picture (like Koriki's ATC-GRID overlays) ---- */

/* c darkened by c >> sh, per channel */
static uint16_t dark565(uint16_t c, int sh)
{
	uint16_t m = (uint16_t)(((31 >> sh) << 11) | ((63 >> sh) << 5) | (31 >> sh));
	return (uint16_t)(c - ((c >> sh) & m));
}

void kf_overlay_grid(uint16_t *px, int pitch, int w, int h, int strength)
{
	/* the ATC overlay: black at 7 % everywhere, 18 % on the lines, every 3
	 * screen pixels. strong ~ the same (1/16, 1/8 + 1/16), soft = about half */
	int base = strength >= 2 ? 4 : 5, line = 3, extra = strength >= 2 ? 4 : 0;
	int x, y;
	if (strength <= 0)
		return;
	for (y = 0; y < h; y++) {
		uint16_t *p = px + y * pitch;
		int ly = (y % 3) == 2;
		for (x = 0; x < w; x++) {
			uint16_t c = p[x];
			if (ly || (x % 3) == 2) {
				uint16_t d = dark565(c, line);
				p[x] = extra ? (uint16_t)(d - ((c >> extra) & (uint16_t)(((31 >> extra) << 11) | ((63 >> extra) << 5) | (31 >> extra)))) : d;
			} else
				p[x] = dark565(c, base);
		}
	}
}
