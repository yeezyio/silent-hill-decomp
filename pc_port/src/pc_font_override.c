/* Unified font atlas uploader (PC port).
 *
 * The localization renderer uses one atlas covering ASCII + extended glyphs
 * (accents/Cyrillic), built by tools/build_unified_font.py. It is a 256x32 image
 * placed at VRAM (0,480): the bottom 16px row holds the base ASCII set (drawn at
 * v=240, same as the stock FONT16 it replaces) and the top row holds the extended
 * glyphs (drawn at v=224). We upload it once, after boot, over that region. */
#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h" /* g_Font16AtlasImg (for the CLUT pos) */
#include "sh_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern s_FsImageDesc g_Font16AtlasImg;

#define UNIFIED_FONT_PATH "Assets/font/Font16Unified.tim"
#define UNIFIED_FONT_VRAM_X 0
#define UNIFIED_FONT_VRAM_Y 480   /* free 16px gap above the stock FONT16 (y496) */

static int s_applied = 0;

static int UploadUnified(void)
{
    FILE*          f;
    long           sz;
    unsigned char* buf;
    unsigned       p, clutLen;
    short          cw, ch, iw, ih;
    unsigned char *clut, *pix;
    RECT           rect;

    f = fopen(UNIFIED_FONT_PATH, "rb");
    if (f == NULL)
        return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 32) { fclose(f); return 0; }
    buf = (unsigned char*)malloc((size_t)sz);
    if (buf == NULL) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    fclose(f);

    if (buf[0] != 0x10) { free(buf); return 0; }
    p       = 8;
    clutLen = *(unsigned*)(buf + p);
    cw      = *(short*)(buf + p + 8);
    ch      = *(short*)(buf + p + 10);
    clut    = buf + p + 12;
    p      += clutLen;
    iw      = *(short*)(buf + p + 8);
    ih      = *(short*)(buf + p + 10);
    pix     = buf + p + 12;

    rect.x = UNIFIED_FONT_VRAM_X;
    rect.y = UNIFIED_FONT_VRAM_Y;
    rect.w = iw;
    rect.h = ih;
    LoadImage(&rect, (u_long*)pix);
    DrawSync(0);

    rect.x = g_Font16AtlasImg.clutX; /* reuse the stock font CLUT slot */
    rect.y = g_Font16AtlasImg.clutY;
    rect.w = cw;
    rect.h = ch;
    LoadImage(&rect, (u_long*)clut);
    DrawSync(0);

    free(buf);
    SH_LOG("[LOC] Unified font atlas uploaded (%dx%d @ %d,%d)", iw, ih,
           UNIFIED_FONT_VRAM_X, UNIFIED_FONT_VRAM_Y);
    return 1;
}

/* Called every frame. Uploads the unified atlas once, after boot, so the stock
 * FONT16 is already resident and we overwrite it with the NotoSans atlas. */
void PcLoc_FontOverrideTick(void)
{
    if (s_applied)
        return;
    if (g_GameWork.gameState < GameState_MainMenu)
        return;
    if (UploadUnified())
        s_applied = 1;
}
