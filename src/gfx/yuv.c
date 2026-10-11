#include "gfx/yuv.h"

#include <malloc.h>
#include <string.h>

/* BT.601 limited range, with y, u, v in 0..1 as sampled from the I8 textures:
 *   R = 1.164y          + 1.596v - 0.8742
 *   G = 1.164y - 0.392u - 0.813v + 0.5319
 *   B = 1.164y + 2.017u          - 1.0855
 * TEV konst/register colours are limited to 0..1, so everything is computed at half scale
 * and the last stage doubles it. Intermediate stages leave clamping off, since TEV registers
 * then hold signed 10-bit values, which leaves room for negatives. Negative per-channel
 * coefficients use the lerp form a + t(b - a), with a = |coef| and b = 0, which yields
 * |coef| - |coef|*t; the leftover constant |coef| is removed in the last stage.
 *
 *   stage 0 (U):  prev  = 2 * (C1 + u(K0 - C1))        C1 = (0, .098, 0)  K0 = (0, 0, .50425)
 *                       = (0, .196 - .196u, 1.0085u)
 *   stage 1 (V):  prev += C0 + v(K1 - C0)              C0 = (0, .4065, 0) K1 = (.798, 0, 0)
 *   stage 2 (Y):  prev += .582y                        K2 = (.582, .582, .582)
 *   stage 3:      out   = clamp(2 * (prev - K3))       K3 = offsets below
 * Half-scale constant offsets: R 0 - (-.4371) = .4371; G (.196 + .4065) - .26595 = .33655;
 * B 0 - (-.54275) = .54275. */
#define C8(x) ((u8)((x) * 255.0f + 0.5f))

static const GXColor k_c0 = {0, C8(0.4065f), 0, 255};
static const GXColor k_c1 = {0, C8(0.098f), 0, 255};
static const GXColor k_k0 = {0, 0, C8(0.50425f), 255};
static const GXColor k_k1 = {C8(0.798f), 0, 0, 255};
static const GXColor k_k2 = {C8(0.582f), C8(0.582f), C8(0.582f), 255};
static const GXColor k_k3 = {C8(0.4371f), C8(0.33655f), C8(0.54275f), 255};

static int round_up(int v, int a)
{
	return (v + a - 1) / a * a;
}

static void init_obj(GXTexObj *o, void *data, int w, int h, int nearest)
{
	GX_InitTexObj(o, data, (u16)w, (u16)h, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	if (nearest)
		GX_InitTexObjFilterMode(o, GX_NEAR, GX_NEAR);
}

int yuv_tex_init(struct yuv_tex *t, int width, int height, int nearest)
{
	memset(t, 0, sizeof *t);
	t->w = width;
	t->h = height;
	t->yw = round_up(width, 8);
	t->yh = round_up(height, 4);
	t->cw = round_up((width + 1) / 2, 8);
	t->ch = round_up((height + 1) / 2, 4);
	t->y = memalign(32, (size_t)t->yw * t->yh);
	t->u = memalign(32, (size_t)t->cw * t->ch);
	t->v = memalign(32, (size_t)t->cw * t->ch);
	if (!t->y || !t->u || !t->v) {
		yuv_tex_free(t);
		return -1;
	}
	init_obj(&t->ty, t->y, t->yw, t->yh, nearest);
	init_obj(&t->tu, t->u, t->cw, t->ch, nearest);
	init_obj(&t->tv, t->v, t->cw, t->ch, nearest);
	return 0;
}

void yuv_tex_free(struct yuv_tex *t)
{
	free(t->y);
	free(t->u);
	free(t->v);
	t->y = t->u = t->v = NULL;
}

/* Linear plane -> GX I8 tiles (8x4 texels, 32 bytes each, tiles in row-major order).
 * Rows past the source height repeat the last row. Columns past the visible width read the
 * decoder's line padding, which FFmpeg guarantees is allocated. */
static void tile_i8(uint8_t *dst, const uint8_t *src, int linesize, int src_h, int tw, int th)
{
	uint64_t *d = (uint64_t *)dst;
	for (int ty = 0; ty < th; ty += 4) {
		const uint64_t *rows[4];
		for (int r = 0; r < 4; r++) {
			int sy = ty + r < src_h ? ty + r : src_h - 1;
			rows[r] = (const uint64_t *)(src + (size_t)sy * linesize);
		}
		for (int tx = 0; tx < tw / 8; tx++) {
			d[0] = rows[0][tx];
			d[1] = rows[1][tx];
			d[2] = rows[2][tx];
			d[3] = rows[3][tx];
			d += 4;
		}
	}
}

void yuv_tex_upload(struct yuv_tex *t, uint8_t *const planes[3], const int linesize[3])
{
	tile_i8(t->y, planes[0], linesize[0], t->h, t->yw, t->yh);
	tile_i8(t->u, planes[1], linesize[1], (t->h + 1) / 2, t->cw, t->ch);
	tile_i8(t->v, planes[2], linesize[2], (t->h + 1) / 2, t->cw, t->ch);
	DCFlushRange(t->y, (u32)t->yw * t->yh);
	DCFlushRange(t->u, (u32)t->cw * t->ch);
	DCFlushRange(t->v, (u32)t->cw * t->ch);
}

void yuv_begin(struct yuv_tex *t)
{
	GX_InvVtxCache();
	GX_InvalidateTexAll();
	GX_LoadTexObj(&t->ty, GX_TEXMAP0);
	GX_LoadTexObj(&t->tu, GX_TEXMAP1);
	GX_LoadTexObj(&t->tv, GX_TEXMAP2);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	GX_SetTevColor(GX_TEVREG0, k_c0);
	GX_SetTevColor(GX_TEVREG1, k_c1);
	GX_SetTevKColor(GX_KCOLOR0, k_k0);
	GX_SetTevKColor(GX_KCOLOR1, k_k1);
	GX_SetTevKColor(GX_KCOLOR2, k_k2);
	GX_SetTevKColor(GX_KCOLOR3, k_k3);

	GX_SetNumTevStages(4);
	static const struct {
		u8 coord, map, ksel, a, d, op, scale, clamp;
	} st[4] = {
		{GX_TEXCOORD1, GX_TEXMAP1, GX_TEV_KCSEL_K0, GX_CC_C1, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_2, GX_FALSE},
		{GX_TEXCOORD1, GX_TEXMAP2, GX_TEV_KCSEL_K1, GX_CC_C0, GX_CC_CPREV, GX_TEV_ADD, GX_CS_SCALE_1, GX_FALSE},
		{GX_TEXCOORD0, GX_TEXMAP0, GX_TEV_KCSEL_K2, GX_CC_ZERO, GX_CC_CPREV, GX_TEV_ADD, GX_CS_SCALE_1, GX_FALSE},
		{GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_TEV_KCSEL_K3, GX_CC_ZERO, GX_CC_CPREV, GX_TEV_SUB, GX_CS_SCALE_2, GX_TRUE},
	};
	for (int i = 0; i < 4; i++) {
		u8 stage = (u8)(GX_TEVSTAGE0 + i);
		GX_SetTevOrder(stage, st[i].coord, st[i].map, GX_COLORNULL);
		GX_SetTevKColorSel(stage, st[i].ksel);
		/* Stage 3 uses lerp(0, K, 1) = K; the others use lerp(a, K, tex). */
		GX_SetTevColorIn(stage, st[i].a, GX_CC_KONST, i == 3 ? GX_CC_ONE : GX_CC_TEXC, st[i].d);
		GX_SetTevColorOp(stage, st[i].op, GX_TB_ZERO, st[i].scale, st[i].clamp, GX_TEVPREV);
		GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_1);
		GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
}

void yuv_draw(struct yuv_tex *t, float x, float y, float w, float h)
{
	float ys = (float)t->w / t->yw, yt = (float)t->h / t->yh;
	float cs = (float)((t->w + 1) / 2) / t->cw, ct = (float)((t->h + 1) / 2) / t->ch;
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position2f32(x, y);
	GX_TexCoord2f32(0, 0);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(x + w, y);
	GX_TexCoord2f32(ys, 0);
	GX_TexCoord2f32(cs, 0);
	GX_Position2f32(x + w, y + h);
	GX_TexCoord2f32(ys, yt);
	GX_TexCoord2f32(cs, ct);
	GX_Position2f32(x, y + h);
	GX_TexCoord2f32(0, yt);
	GX_TexCoord2f32(0, ct);
	GX_End();
}
