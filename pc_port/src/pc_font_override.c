/* Per-locale font atlas override (PC port).
 *
 * A locale may ship a replacement FONT16 atlas (Assets/Locales/<Name>/Font16.tim)
 * — e.g. the Russian Cyrillic codepage. The stock FONT16 is loaded from the disc
 * into VRAM during boot; once we're past boot we upload the locale's atlas over
 * the same VRAM rects (derived from g_Font16AtlasImg exactly as the loader does),
 * so the existing 12x16 renderer draws the new glyphs unchanged. */
#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h" /* g_Font16AtlasImg */
#include "main/fsqueue.h"                 /* s_FsImageDesc */
#include "sh_log.h"
#include "pc_locale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern s_FsImageDesc g_Font16AtlasImg;

static int s_appliedGen = -1;

static int UploadFontTim(const char* path)
{
    FILE*          f;
    long           sz;
    unsigned char* buf;
    unsigned       p, clutLen, imgLen;
    short          cw, ch, iw, ih;
    unsigned char *clut, *pix;
    RECT           rect;

    f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 32) { fclose(f); return 0; }
    buf = (unsigned char*)malloc((size_t)sz);
    if (buf == NULL) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    fclose(f);

    /* TIM: magic(4) + flag(4), then CLUT block and image block, each
     * len(4) + x,y,w,h (4*u16) + data. */
    if (buf[0] != 0x10) { free(buf); return 0; }
    p       = 8;
    clutLen = *(unsigned*)(buf + p);
    cw      = *(short*)(buf + p + 8);
    ch      = *(short*)(buf + p + 10);
    clut    = buf + p + 12;
    p      += clutLen;
    imgLen  = *(unsigned*)(buf + p);
    iw      = *(short*)(buf + p + 8);
    ih      = *(short*)(buf + p + 10);
    pix     = buf + p + 12;
    (void)imgLen;

    /* VRAM destination, derived from the atlas descriptor the same way
     * Fs_QueueTickRead (fsqueue_3.c) does for the native upload. */
    rect.x = g_Font16AtlasImg.u + ((g_Font16AtlasImg.tPage[1] & 0xF) << 6);
    rect.y = g_Font16AtlasImg.v + ((g_Font16AtlasImg.tPage[1] << 4) & 0x100);
    rect.w = iw;
    rect.h = ih;
    LoadImage(&rect, (u_long*)pix);
    DrawSync(0);

    rect.x = g_Font16AtlasImg.clutX;
    rect.y = g_Font16AtlasImg.clutY;
    rect.w = cw;
    rect.h = ch;
    LoadImage(&rect, (u_long*)clut);
    DrawSync(0);

    free(buf);
    SH_LOG("[LOC] Font override applied: %s", path);
    return 1;
}

/* Called every frame. Uploads the active locale's replacement font once per
 * activation, after boot (so the stock FONT16 is already in VRAM). */
void PcLoc_FontOverrideTick(void)
{
    const char* path;

    /* Wait until past the logo/boot states so FONT16 is loaded in VRAM. */
    if (g_GameWork.gameState < GameState_MainMenu)
        return;
    if (s_appliedGen == Loc_Generation())
        return;

    path = Loc_ActiveFontPath();
    if (path[0] != '\0')
    {
        if (UploadFontTim(path))
            s_appliedGen = Loc_Generation();
    }
    else
    {
        /* No override for this locale. (Switching from a custom-font locale back
         * to a stock one needs a restart to restore the Latin atlas.) */
        s_appliedGen = Loc_Generation();
    }
}
