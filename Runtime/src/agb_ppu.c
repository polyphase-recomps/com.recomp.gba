/*
 * GBA PPU, one scanline at a time: text and affine backgrounds, bitmap modes 3-5,
 * regular and affine sprites, windows 0/1/OBJ, alpha blending and brightness, mosaic.
 * Reads the registers straight from agb_io, so HBlank DMA and HBlank IRQ effects
 * between lines behave as on the console.
 *
 * This is most of a frame's work (and all of it on slow consoles), so the inner loops
 * work per 8-pixel tile row rather than per pixel, registers are read once per line, the
 * enabled backgrounds are sorted by priority once per line, and the compositor stops at
 * the first opaque layer when nothing on the line can blend.
 */
#include "agb.h"

#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed short s16;
typedef signed int s32;

#define IO16(o) AGB_IO16(o)
#define IO32(o) AGB_IO32(o)

#define TRANSPARENT 0x8000u /* in line buffers: pixel not drawn */

/* Affine background reference points, advanced per line. */
static s32 sRefX[2], sRefY[2];
static u32 sLatchX[2], sLatchY[2];

static s32 sign28(u32 v)
{
    return ((s32)(v << 4)) >> 4;
}

#define PLTT ((const u16 *)agb_pltt)
#define PLTT_BG(i) (PLTT[(i)] & 0x7FFF)
#define PLTT_OBJ(i) (PLTT[256 + (i)] & 0x7FFF)

/* Called at line 0 and whenever the game rewrites BGxX/BGxY. */
static void affine_refresh(int line)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        u32 x = IO32(0x28 + i * 0x10), y = IO32(0x2C + i * 0x10);

        if (line == 0 || x != sLatchX[i])
        {
            sRefX[i] = sign28(x);
            sLatchX[i] = x;
        }
        if (line == 0 || y != sLatchY[i])
        {
            sRefY[i] = sign28(y);
            sLatchY[i] = y;
        }
    }
}

static void affine_advance(void)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        sRefX[i] += (s16)IO16(0x22 + i * 0x10); /* PB */
        sRefY[i] += (s16)IO16(0x26 + i * 0x10); /* PD */
    }
}

static void render_text_bg(int bg, int line, u16 *out)
{
    u16 cnt = IO16(0x08 + bg * 2);
    int hofs = IO16(0x10 + bg * 4) & 0x1FF, vofs = IO16(0x12 + bg * 4) & 0x1FF;
    u32 charBase = ((cnt >> 2) & 3) * 0x4000;
    const u8 *chars = agb_vram + charBase;
    const u16 *screen = (const u16 *)(agb_vram + ((cnt >> 8) & 0x1F) * 0x800);
    int color256 = (cnt >> 7) & 1;
    int size = (cnt >> 14) & 3;
    int wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    int y = (line + vofs) & hmask;
    int mx = 1;
    const u16 *row;
    int x;

    if (cnt & 0x40) /* mosaic */
    {
        u16 mosaic = IO16(0x4C);
        int my = ((mosaic >> 4) & 0xF) + 1;

        mx = (mosaic & 0xF) + 1;
        y = ((line - line % my) + vofs) & hmask;
    }
    /* the map row for this line: blocks of 32x32 entries, the second row of blocks for y >= 256 */
    row = screen + ((y >= 256) ? ((size == 3) ? 2 : 1) * 1024 : 0) + ((y & 255) >> 3) * 32;

    if (mx > 1)
    {
        /* horizontal mosaic: one sample per mx pixels */
        for (x = 0; x < 240; x++)
        {
            int px = ((x - x % mx) + hofs) & wmask;
            u16 entry = row[(px >= 256 ? 1024 : 0) + ((px & 255) >> 3)];
            int tile = entry & 0x3FF, tx = px & 7, ty = y & 7, color;

            if (entry & 0x400) tx = 7 - tx;
            if (entry & 0x800) ty = 7 - ty;
            if (color256)
            {
                u32 off = tile * 64 + ty * 8 + tx;

                if (charBase + off >= 0x10000) { out[x] = TRANSPARENT; continue; }
                color = chars[off];
                out[x] = color ? PLTT_BG(color) : TRANSPARENT;
            }
            else
            {
                u32 off = tile * 32 + ty * 4 + (tx >> 1);

                if (charBase + off >= 0x10000) { out[x] = TRANSPARENT; continue; }
                color = (chars[off] >> ((tx & 1) * 4)) & 0xF;
                out[x] = color ? PLTT_BG((entry >> 12) * 16 + color) : TRANSPARENT;
            }
        }
        return;
    }

    /* one map entry and one tile row per (up to) 8 pixels */
    for (x = 0; x < 240;)
    {
        int px = (x + hofs) & wmask;
        u16 entry = row[(px >= 256 ? 1024 : 0) + ((px & 255) >> 3)];
        int tile = entry & 0x3FF, tx = px & 7, ty = y & 7;
        int n = 8 - tx, k;
        int flipx = (entry & 0x400) != 0;

        if (n > 240 - x) n = 240 - x;
        if (entry & 0x800) ty = 7 - ty;
        if (color256)
        {
            u32 off = tile * 64 + ty * 8;
            u8 px8[8];

            if (charBase + off >= 0x10000)
            {
                for (k = 0; k < n; k++) out[x + k] = TRANSPARENT;
                x += n;
                continue;
            }
            memcpy(px8, chars + off, 8);
            for (k = 0; k < n; k++, tx++)
            {
                int c = px8[flipx ? 7 - tx : tx];

                out[x + k] = c ? PLTT_BG(c) : TRANSPARENT;
            }
        }
        else
        {
            u32 off = tile * 32 + ty * 4;
            int pal = (entry >> 12) * 16;
            u8 px4[4];

            if (charBase + off >= 0x10000)
            {
                for (k = 0; k < n; k++) out[x + k] = TRANSPARENT;
                x += n;
                continue;
            }
            memcpy(px4, chars + off, 4);
            for (k = 0; k < n; k++, tx++)
            {
                int t = flipx ? 7 - tx : tx;
                int c = (px4[t >> 1] >> ((t & 1) * 4)) & 0xF;

                out[x + k] = c ? PLTT_BG(pal + c) : TRANSPARENT;
            }
        }
        x += n;
    }
}

static void render_affine_bg(int bg, u16 *out)
{
    int i = bg - 2;
    u16 cnt = IO16(0x08 + bg * 2);
    const u8 *chars = agb_vram + ((cnt >> 2) & 3) * 0x4000;
    const u8 *screen = agb_vram + ((cnt >> 8) & 0x1F) * 0x800;
    int sizePx = 128 << ((cnt >> 14) & 3);
    int wrap = (cnt >> 13) & 1;
    s16 pa = (s16)IO16(0x20 + i * 0x10), pc = (s16)IO16(0x24 + i * 0x10);
    s32 fx = sRefX[i], fy = sRefY[i];
    int x;

    for (x = 0; x < 240; x++, fx += pa, fy += pc)
    {
        int px = fx >> 8, py = fy >> 8;
        int tile, color;

        if (wrap)
        {
            px &= sizePx - 1;
            py &= sizePx - 1;
        }
        else if ((unsigned)px >= (unsigned)sizePx || (unsigned)py >= (unsigned)sizePx)
        {
            out[x] = TRANSPARENT;
            continue;
        }
        tile = screen[(py >> 3) * (sizePx >> 3) + (px >> 3)];
        color = chars[tile * 64 + (py & 7) * 8 + (px & 7)];
        out[x] = color ? PLTT_BG(color) : TRANSPARENT;
    }
}

static void render_bitmap_bg(int mode, u16 *out)
{
    s16 pa = (s16)IO16(0x20), pc = (s16)IO16(0x24);
    s32 fx = sRefX[0], fy = sRefY[0];
    int frame = (IO16(0x00) >> 4) & 1;
    int w = mode == 5 ? 160 : 240, h = mode == 5 ? 128 : 160;
    int x;

    for (x = 0; x < 240; x++, fx += pa, fy += pc)
    {
        int px = fx >> 8, py = fy >> 8;

        if (px < 0 || py < 0 || px >= w || py >= h)
        {
            out[x] = TRANSPARENT;
            continue;
        }
        if (mode == 4)
        {
            u8 c = agb_vram[frame * 0xA000 + py * 240 + px];

            out[x] = c ? PLTT_BG(c) : TRANSPARENT;
        }
        else if (mode == 3)
        {
            out[x] = ((const u16 *)agb_vram)[py * 240 + px] & 0x7FFF;
        }
        else
        {
            out[x] = ((const u16 *)(agb_vram + frame * 0xA000))[py * 160 + px] & 0x7FFF;
        }
    }
}

/* Sprite line: colour, priority, flags (semi-transparent / OBJ window) per pixel. */
static u16 sObjColor[240];
static u8 sObjPrio[240];
static u8 sObjSemi[240];
static u8 sObjWin[240];
static int sObjSemiOnLine; /* any semi-transparent sprite pixel on this line */
static int sObjWinOnLine;  /* any OBJ-window pixel on this line */

static const u8 kObjSize[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

/* Puts one sprite pixel (palette index `color` already offset for 4bpp) into the line. */
#define OBJ_PIXEL(screenX, color)                                                         \
    do                                                                                    \
    {                                                                                     \
        if (mode == 2)                                                                    \
        {                                                                                 \
            sObjWin[screenX] = 1;                                                         \
            sObjWinOnLine = 1;                                                            \
        }                                                                                 \
        else if (sObjColor[screenX] == TRANSPARENT || prio < sObjPrio[screenX])           \
        {                                                                                 \
            /* lower OAM index wins on equal priority: only replace with strictly better */ \
            sObjColor[screenX] = PLTT_OBJ(color);                                         \
            sObjPrio[screenX] = (u8)prio;                                                 \
            sObjSemi[screenX] = mode == 1;                                                \
            if (mode == 1) sObjSemiOnLine = 1;                                            \
        }                                                                                 \
    } while (0)

static void render_sprites(int line, u16 dispcnt)
{
    const u16 *oam = (const u16 *)agb_oam;
    int map1d = (dispcnt >> 6) & 1;
    int bitmapMode = (dispcnt & 7) >= 3;
    u16 mosaic = IO16(0x4C);
    int omx = ((mosaic >> 8) & 0xF) + 1, omy = ((mosaic >> 12) & 0xF) + 1;
    int i;

    for (i = 0; i < 240; i++) sObjColor[i] = TRANSPARENT;
    memset(sObjPrio, 4, sizeof(sObjPrio));
    memset(sObjSemi, 0, sizeof(sObjSemi));
    memset(sObjWin, 0, sizeof(sObjWin));
    sObjSemiOnLine = sObjWinOnLine = 0;
    if (!(dispcnt & 0x1000)) return;

    for (i = 0; i < 128; i++)
    {
        u16 a0 = oam[i * 4], a1, a2;
        int affine = (a0 >> 8) & 1, dbl = (a0 >> 9) & 1;
        int mode = (a0 >> 10) & 3, color256 = (a0 >> 13) & 1, shape = (a0 >> 14) & 3;
        int size, w, h, bw, bh, y, x, sx, tile, pal, prio, ly;

        if (!affine && dbl) continue; /* disabled */
        if (shape == 3 || mode == 3) continue;
        y = a0 & 0xFF;
        if (y >= 160) y -= 256;
        if (line < y) continue;
        a1 = oam[i * 4 + 1];
        size = (a1 >> 14) & 3;
        w = kObjSize[shape][size][0];
        h = kObjSize[shape][size][1];
        bw = w;
        bh = h;
        if (affine && dbl)
        {
            bw *= 2;
            bh *= 2;
        }
        if (line >= y + bh) continue;
        x = a1 & 0x1FF;
        if (x >= 240) x -= 512;
        if (x + bw <= 0) continue;
        a2 = oam[i * 4 + 2];
        tile = a2 & 0x3FF;
        prio = (a2 >> 10) & 3;
        pal = a2 >> 12;
        if (bitmapMode && tile < 512) continue;
        ly = line - y;

        if (affine || (a0 & 0x1000))
        {
            /* affine or mosaic: per pixel */
            s16 pa = 256, pb = 0, pc = 0, pd = 256;

            if (affine)
            {
                int p = (a1 >> 9) & 0x1F;

                pa = (s16)oam[p * 16 + 3];
                pb = (s16)oam[p * 16 + 7];
                pc = (s16)oam[p * 16 + 11];
                pd = (s16)oam[p * 16 + 15];
            }
            for (sx = 0; sx < bw; sx++)
            {
                int screenX = x + sx;
                int tx, ty, color;
                int lx = sx, py = ly; /* position inside the sprite box */
                u32 tileIndex, off;

                if (screenX < 0 || screenX >= 240) continue;
                if (a0 & 0x1000) /* OBJ mosaic, in screen coordinates */
                {
                    lx = sx - screenX % omx;
                    py = (line - line % omy) - y;
                    if (lx < 0) lx = 0;
                    if (py < 0) py = 0;
                }
                if (affine)
                {
                    int cx = lx - bw / 2, cy = py - bh / 2;

                    tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                    ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                    if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
                }
                else
                {
                    tx = lx;
                    ty = py;
                    if (a1 & 0x1000) tx = w - 1 - tx;
                    if (a1 & 0x2000) ty = h - 1 - ty;
                }
                if (color256)
                {
                    if (map1d)
                        tileIndex = tile + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2;
                    else
                        tileIndex = (tile & ~1) + (ty >> 3) * 32 + (tx >> 3) * 2;
                    off = 0x10000 + (tileIndex & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7);
                    if (off >= AGB_VRAM_SIZE) continue;
                    color = agb_vram[off];
                    if (!color) continue;
                }
                else
                {
                    if (map1d)
                        tileIndex = tile + (ty >> 3) * (w >> 3) + (tx >> 3);
                    else
                        tileIndex = tile + (ty >> 3) * 32 + (tx >> 3);
                    off = 0x10000 + (tileIndex & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
                    if (off >= AGB_VRAM_SIZE) continue;
                    color = (agb_vram[off] >> ((tx & 1) * 4)) & 0xF;
                    if (!color) continue;
                    color += pal * 16;
                }
                OBJ_PIXEL(screenX, color);
            }
        }
        else
        {
            /* regular sprite: one tile row per 8 pixels */
            int ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            int flipx = (a1 & 0x1000) != 0;
            int sx0 = x < 0 ? -x : 0, sx1 = x + bw > 240 ? 240 - x : bw;

            for (sx = sx0; sx < sx1;)
            {
                int tx = flipx ? w - 1 - sx : sx;
                int col = tx >> 3;
                /* pixels of this tile column, in screen order */
                int n = flipx ? (tx & 7) + 1 : 8 - (tx & 7);
                u32 tileIndex, off;
                int k;

                if (n > sx1 - sx) n = sx1 - sx;
                if (color256)
                {
                    u8 row[8];

                    if (map1d)
                        tileIndex = tile + ((ty >> 3) * (w >> 3) + col) * 2;
                    else
                        tileIndex = (tile & ~1) + (ty >> 3) * 32 + col * 2;
                    off = 0x10000 + (tileIndex & 0x3FF) * 32 + (ty & 7) * 8;
                    if (off >= AGB_VRAM_SIZE)
                    {
                        sx += n;
                        continue;
                    }
                    memcpy(row, agb_vram + off, 8);
                    for (k = 0; k < n; k++)
                    {
                        int t = (flipx ? tx - k : tx + k) & 7;
                        int color = row[t];

                        if (color) OBJ_PIXEL(x + sx + k, color);
                    }
                }
                else
                {
                    u8 row[4];

                    if (map1d)
                        tileIndex = tile + (ty >> 3) * (w >> 3) + col;
                    else
                        tileIndex = tile + (ty >> 3) * 32 + col;
                    off = 0x10000 + (tileIndex & 0x3FF) * 32 + (ty & 7) * 4;
                    if (off >= AGB_VRAM_SIZE)
                    {
                        sx += n;
                        continue;
                    }
                    memcpy(row, agb_vram + off, 4);
                    for (k = 0; k < n; k++)
                    {
                        int t = (flipx ? tx - k : tx + k) & 7;
                        int color = (row[t >> 1] >> ((t & 1) * 4)) & 0xF;

                        if (color) OBJ_PIXEL(x + sx + k, color + pal * 16);
                    }
                }
                sx += n;
            }
        }
    }
}

/* Window masks of a line: bit0-3 BG0-3, bit4 OBJ, bit5 colour effects. */
static void window_masks(int line, u16 dispcnt, u8 *mask)
{
    u16 winin = IO16(0x48), winout = IO16(0x4A);
    u8 outside = (u8)(winout & 0x3F), objwin = (u8)((winout >> 8) & 0x3F);
    int w, x;

    if (!(dispcnt & 0xE000))
    {
        memset(mask, 0x3F, 240);
        return;
    }
    /* outside / OBJ window first, then the windows 1 and 0 on top (window 0 wins) */
    if ((dispcnt & 0x8000) && sObjWinOnLine)
    {
        for (x = 0; x < 240; x++) mask[x] = sObjWin[x] ? objwin : outside;
    }
    else
    {
        memset(mask, outside, 240);
    }
    for (w = 1; w >= 0; w--)
    {
        u16 hreg, vreg;
        int x1, x2, y1, y2, inY;
        u8 inside;

        if (!(dispcnt & (0x2000 << w))) continue;
        hreg = IO16(0x40 + w * 2);
        vreg = IO16(0x44 + w * 2);
        x1 = hreg >> 8;
        x2 = hreg & 0xFF;
        y1 = vreg >> 8;
        y2 = vreg & 0xFF;
        if (x2 > 240 || x1 > x2) x2 = 240;
        if (y2 > 160 || y1 > y2) y2 = 160;
        inY = (y1 <= y2) ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
        if (!inY) continue;
        inside = (u8)((winin >> (w * 8)) & 0x3F);
        for (x = 0; x < 240; x++)
        {
            int inX = (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);

            if (inX) mask[x] = inside;
        }
    }
}

static u16 blend_alpha(u16 a, u16 b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;

    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (u16)(r | (g << 5) | (bl << 10));
}

static u16 brighten(u16 c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}

static u16 darken(u16 c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}

void agb_ppu_render_line(int line, u16 *out)
{
    static u16 bgLine[4][240];
    static u8 winMask[240];
    u16 dispcnt = IO16(0x00);
    int mode = dispcnt & 7;
    int bgEnabled[4] = {0, 0, 0, 0};
    int bgPrio[4];
    int order[4], count = 0; /* enabled backgrounds by (priority, index) */
    u16 bldcnt = IO16(0x50);
    int effect = (bldcnt >> 6) & 3;
    int eva = IO16(0x52) & 0x1F, evb = (IO16(0x52) >> 8) & 0x1F, evy = IO16(0x54) & 0x1F;
    u16 backdrop = PLTT_BG(0);
    int needSecond;
    int i, j, x;

    affine_refresh(line);

    if (dispcnt & 0x80)
    {
        for (x = 0; x < 240; x++) out[x] = 0x7FFF;
        affine_advance();
        return;
    }
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;

    for (i = 0; i < 4; i++)
    {
        if (!(dispcnt & (0x100 << i))) continue;
        bgPrio[i] = IO16(0x08 + i * 2) & 3;
        switch (mode)
        {
        case 0:
            render_text_bg(i, line, bgLine[i]);
            bgEnabled[i] = 1;
            break;
        case 1:
            if (i < 2) render_text_bg(i, line, bgLine[i]);
            else if (i == 2) render_affine_bg(2, bgLine[i]);
            else break;
            bgEnabled[i] = 1;
            break;
        case 2:
            if (i >= 2)
            {
                render_affine_bg(i, bgLine[i]);
                bgEnabled[i] = 1;
            }
            break;
        default:
            if (i == 2)
            {
                render_bitmap_bg(mode, bgLine[i]);
                bgEnabled[i] = 1;
            }
            break;
        }
    }
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++)
            if (bgEnabled[i] && bgPrio[i] == j) order[count++] = i;

    render_sprites(line, dispcnt);
    window_masks(line, dispcnt, winMask);
    /* the second layer only matters for alpha blending */
    needSecond = effect == 1 || sObjSemiOnLine;

    for (x = 0; x < 240; x++)
    {
        u8 win = winMask[x];
        /* top two layers: layer ids 0-3 BG, 4 OBJ, 5 backdrop */
        u16 col[2] = {backdrop, backdrop};
        int layer[2] = {5, 5};
        int found = 0, k;
        int objPrio = ((win & 0x10) && sObjColor[x] != TRANSPARENT) ? sObjPrio[x] : 4;
        int limit = needSecond ? 2 : 1;

        for (k = 0; k < count && found < limit; k++)
        {
            int b = order[k];
            u16 c;

            if (objPrio <= bgPrio[b])
            {
                /* the sprite comes before the backgrounds of its priority and below */
                col[found] = sObjColor[x];
                layer[found] = 4;
                found++;
                objPrio = 4;
                if (found >= limit) break;
            }
            if (!(win & (1 << b))) continue;
            c = bgLine[b][x];
            if (c == TRANSPARENT) continue;
            col[found] = c;
            layer[found] = b;
            found++;
        }
        if (found < limit && objPrio < 4)
        {
            col[found] = sObjColor[x];
            layer[found] = 4;
            found++;
        }

        {
            u16 c = col[0];
            int top = layer[0];

            if (top == 4 && sObjSemi[x] && ((bldcnt >> (8 + layer[1])) & 1))
            {
                c = blend_alpha(col[0], col[1], eva, evb);
            }
            else if (effect && (win & 0x20) && ((bldcnt >> top) & 1))
            {
                if (effect == 1)
                {
                    if ((bldcnt >> (8 + layer[1])) & 1) c = blend_alpha(col[0], col[1], eva, evb);
                }
                else if (effect == 2)
                {
                    c = brighten(c, evy);
                }
                else
                {
                    c = darken(c, evy);
                }
            }
            out[x] = c;
        }
    }
    affine_advance();
}
