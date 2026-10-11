#include "gfx/gx.h"

#include <malloc.h>
#include <string.h>

#define FIFO_SIZE (256 * 1024)

void gx_init(struct gx_state *g)
{
	VIDEO_Init();
	g->mode = VIDEO_GetPreferredMode(NULL);
	g->xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g->mode));
	g->xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g->mode));
	g->cur = 0;
	VIDEO_Configure(g->mode);
	VIDEO_SetNextFramebuffer(g->xfb[0]);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (g->mode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	void *fifo = memalign(32, FIFO_SIZE);
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);

	GXColor black = {0, 0, 0, 255};
	GX_SetCopyClear(black, GX_MAX_Z24);
	GX_SetViewport(0, 0, g->mode->fbWidth, g->mode->efbHeight, 0, 1);
	f32 yscale = GX_GetYScaleFactor(g->mode->efbHeight, g->mode->xfbHeight);
	u32 xfb_h = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, g->mode->fbWidth, g->mode->efbHeight);
	GX_SetDispCopySrc(0, 0, g->mode->fbWidth, g->mode->efbHeight);
	GX_SetDispCopyDst(g->mode->fbWidth, xfb_h);
	GX_SetCopyFilter(g->mode->aa, g->mode->sample_pattern, GX_TRUE, g->mode->vfilter);
	GX_SetFieldMode(g->mode->field_rendering,
		(g->mode->viHeight == 2 * g->mode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(g->mode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(g->xfb[0], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	Mtx44 proj;
	guOrtho(proj, 0, 479, 0, 639, 0, 300);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	Mtx mv;
	guMtxIdentity(mv);
	/* 2D vertices have z = 0, which sits on the ortho near plane and gets clipped;
	 * push everything into the view volume (GRRLIB does the same). */
	guMtxTransApply(mv, mv, 0.0f, 0.0f, -100.0f);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
}

void gx_present(struct gx_state *g, int vsync)
{
	g->cur ^= 1;
	GX_CopyDisp(g->xfb[g->cur], GX_TRUE);
	GX_DrawDone();
	VIDEO_SetNextFramebuffer(g->xfb[g->cur]);
	VIDEO_Flush();
	if (vsync)
		VIDEO_WaitVSync();
}

void *gx_front_xfb(struct gx_state *g)
{
	return g->xfb[g->cur];
}
