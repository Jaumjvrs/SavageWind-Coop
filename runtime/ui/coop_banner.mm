// (split map, 2026-11-27) The empty quarter of a three-player split screen (the banner process, runtime/coop_split.cpp):
// a map of the overworld with every player's dot, facing and name, and a party panel (hearts, stamina, and where they are
// when not on the overworld). Each game writes its player's line into the window's shared block (runtime/coop_split_hud.h,
// runtime/coop/splithud.inc); with no line yet the banner keeps its picture (runtime/ui/launcher.mm boot_splash_draw).
// The map pictures are the game's own: UI/MapTex/MainField/MapTex<L>_<col>-<row>.sbmaptex, one per 1000 m square
// (columns Z,A..K from x -6000, rows 0..9 from z -5000; north is -z), a Yaz0 BFRES whose BNTX holds one BC1 texture,
// block linear. Level 3: 135 px a square (all 120 make the whole-map picture); level 1: 840 px (the squares in view
// when zoomed in, made as needed).
// (map fog, 2026-11-27, user: unrevealed regions "kinda blue, like when you dont have it unlocked") The game draws a
// region whose Sheikah Tower is off in its blueprint look, from files beside each square: "_<m>" = the square with only
// the towers in bit mask m revealed (bit n: MapTower_<n+1>; "_0": all blueprint), no suffix = all its towers revealed.
// k_sq_towers says which towers' regions touch each square (from the file names; one-tower squares from the game's
// tower region grid, Ecosystem/MapTower.sbeco). The revealed towers: the union of the players' (their lines carry
// them, runtime/coop/splithud.inc); the whole map again with SAVAGEWIND_COOP_MAPFOG=0. The "-<n>" variants (Tarrey
// Town's growth) are not used.
// The view follows the players: centred on them, zoomed out as they spread, never closer than player 1's Options >
// Co-op > Map zoom (Near / Normal / Far: the user's girlfriend's idea).
#include <SDL3/SDL.h>
#include "imgui.h"
#include "i18n.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
extern "C" {
#include "../rt.h"
#include "../coop_split_hud.h"
#include "../services/archive.h"
#include "../nvn/tegra.h"
void gpu_lock(void);
void gpu_unlock(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
void vk_imgui_texture_free(uint64_t id);
void *boot_splash_context(void);
}

// (GPU lock held) ImGui's Vulkan backend keeps its textures in the boot picture's context: made current around each
// upload and free (the main thread has none of its own)
struct CtxScope {
    ImGuiContext *prev = ImGui::GetCurrentContext();
    bool ok = boot_splash_context() != NULL;
    CtxScope() { if (ok) ImGui::SetCurrentContext((ImGuiContext *)boot_splash_context()); }
    ~CtxScope() { if (ok) ImGui::SetCurrentContext(prev); }
};

extern "C" void coop_banner_init(const char *root);
extern "C" void coop_banner_tick(void);
extern "C" bool coop_banner_draw(ImDrawList *dl, ImVec2 ds, float s, ImFont *font, float dt);

#ifndef __APPLE__
#define MAP_COLS 12
#define MAP_ROWS 10
#define MAP_X0 -6000.0f
#define MAP_Z0 -5000.0f
#define BASE_PX 135
#define DETAIL_PX 840
#define DETAIL_MAX 20
#define STALE_MS 3000

static char g_dir[1024], g_romfs[1024];
static bool g_on;
// (GPU lock: the drawing thread and the main thread's tick share these)
static uint64_t g_base;  // the whole map, level 3
static bool g_base_tried;
static uint32_t g_towers = SPLIT_TOWERS_ALL;  // the revealed towers the map shows (bit n: MapTower_<n+1>)
static bool g_towers_known;  // (false: no player's towers read yet: no map yet)
static uint32_t g_base_towers = UINT32_MAX;  // the towers g_base was made with
static uint64_t g_hearts;  // the game's heart pictures, white: [0] the shape, [1..3] 1-3 quarters filled, [4] full
#define HEART_PX 48
static uint64_t g_stamps;  // the game's 9 stamp pictures in a row (No 27..35)
#define STAMP_PX 32
static uint64_t g_pins;  // the game's 5 map pin glyphs in a row (No 37..41), white
#define PIN_PX 64
struct Detail { int col, row; uint32_t shown; uint64_t tex, used; };
static std::vector<Detail> g_detail;
static int g_want[16][2], g_nwant;
static uint64_t g_frame;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static const uint8_t *find4(const uint8_t *p, const uint8_t *end, const char *magic) {
    for (; p + 4 <= end; p++)
        if (!memcmp(p, magic, 4)) return p;
    return NULL;
}

// The towers whose regions touch each map square: [row][column Z, A..K]
static const uint16_t k_sq_towers[MAP_ROWS][MAP_COLS] = {
    {0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0011, 0x0210, 0x0210, 0x0200, 0x0200, 0x0600, 0x0600},
    {0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0011, 0x0210, 0x0610, 0x0600, 0x0600, 0x0600, 0x0400},
    {0x0003, 0x0003, 0x0003, 0x0003, 0x0011, 0x0011, 0x0010, 0x0210, 0x0600, 0x0600, 0x0400, 0x0c00},
    {0x0003, 0x0003, 0x4003, 0x4013, 0x4031, 0x4030, 0x0030, 0x0630, 0x0600, 0x0c00, 0x0c00, 0x0c00},
    {0x0006, 0x4006, 0x4002, 0x4000, 0x4020, 0x0020, 0x0820, 0x0a30, 0x0e00, 0x0c00, 0x0c00, 0x0800},
    {0x0006, 0x4004, 0x4004, 0x4004, 0x4020, 0x0020, 0x08a0, 0x1880, 0x1880, 0x1800, 0x1800, 0x0800},
    {0x0006, 0x000c, 0x000c, 0x400c, 0x406c, 0x00e0, 0x01a0, 0x1880, 0x1080, 0x1000, 0x3800, 0x3800},
    {0x000c, 0x000c, 0x000c, 0x000c, 0x014c, 0x01c0, 0x2180, 0x2180, 0x3080, 0x3000, 0x3000, 0x2000},
    {0x0008, 0x0008, 0x0008, 0x0008, 0x0108, 0x0108, 0x2100, 0x2100, 0x2000, 0x3000, 0x3000, 0x2000},
    {0x0008, 0x0008, 0x0008, 0x0008, 0x0108, 0x0108, 0x2100, 0x2100, 0x2000, 0x2000, 0x2000, 0x2000},
};
// Which picture of a square to show: its revealed towers (k_sq_towers bits), the whole square when all are
static uint32_t sq_shown(int col, int row, uint32_t towers) {
    return towers & k_sq_towers[row][col];
}

// One map square as RGBA (w*w*4 bytes into out), false when the file is missing or not what is expected. shown:
// sq_shown (the square's own towers all in it: the plain revealed picture)
static bool load_square(int level, int col, int row, int w, uint8_t *out, int out_stride, uint32_t shown) {
    char path[1200];
    int pn = snprintf(path, sizeof path, "%s/MapTex%d_%c-%d", g_dir, level, "ZABCDEFGHIJK"[col], row);
    if (shown != k_sq_towers[row][col] && pn > 0 && pn < (int)sizeof path - 24)
        snprintf(path + pn, sizeof path - pn, "_%u", (unsigned)shown);
    snprintf(path + strlen(path), sizeof path - strlen(path), ".sbmaptex");
    FILE *f = fopen(path, "rb");
    if (!f && shown != k_sq_towers[row][col]) return load_square(level, col, row, w, out, out_stride, k_sq_towers[row][col]);
    if (!f) return false;
    std::vector<uint8_t> z;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) z.insert(z.end(), buf, buf + got);
    fclose(f);
    size_t n = 0;
    uint8_t *d = z.size() > 16 ? yaz0_unpack(z.data(), z.size(), &n) : NULL;
    if (!d) return false;
    bool ok = false;
    const uint8_t *end = d + n, *k = find4(d, end, "BNTX");
    const uint8_t *r = k ? find4(k, end, "BRTI") : NULL, *t = k ? find4(k, end, "BRTD") : NULL;
    if (r && t && r + 0x38 <= end) {
        uint32_t fmt = rd32(r + 0x1c), iw = rd32(r + 0x24), ih = rd32(r + 0x28), bh = rd32(r + 0x34);
        uint32_t bw = (iw + 3) / 4, bhk = (ih + 3) / 4;
        uint64_t gob_rows = 8ull << (bh < 6 ? bh : 0), rows = (bhk + gob_rows - 1) / gob_rows * gob_rows;
        uint64_t size = ((uint64_t)bw * 8 + 63) / 64 * 64 * rows;
        if (fmt == 0x1a06 && (int)iw == w && (int)ih == w && bh < 6 && (uint64_t)(end - t) >= 16 + size) {
            std::vector<uint8_t> lin((size_t)bw * bhk * 8);
            tegra_deswizzle(t + 16, lin.data(), bw, bhk, 1, 8, bh, 0);
            for (uint32_t by = 0; by < bhk; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {  // BC1: two RGB565 colours and 2 bits a pixel
                    const uint8_t *b = &lin[((size_t)by * bw + bx) * 8];
                    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
                    uint32_t bits = rd32(b + 4);
                    uint8_t pal[4][3];
                    for (int i = 0; i < 2; i++) {
                        unsigned c = i ? c1 : c0;
                        pal[i][0] = (uint8_t)((c >> 11 & 31) * 255 / 31);
                        pal[i][1] = (uint8_t)((c >> 5 & 63) * 255 / 63);
                        pal[i][2] = (uint8_t)((c & 31) * 255 / 31);
                    }
                    for (int j = 0; j < 3; j++) {
                        if (c0 > c1) {
                            pal[2][j] = (uint8_t)((2 * pal[0][j] + pal[1][j]) / 3);
                            pal[3][j] = (uint8_t)((pal[0][j] + 2 * pal[1][j]) / 3);
                        } else {
                            pal[2][j] = (uint8_t)((pal[0][j] + pal[1][j]) / 2);
                            pal[3][j] = 0;
                        }
                    }
                    for (int i = 0; i < 16; i++) {
                        uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                        if (x >= iw || y >= ih) continue;
                        uint8_t *px = out + (size_t)y * out_stride + (size_t)x * 4;
                        const uint8_t *c = pal[bits >> (2 * i) & 3];
                        px[0] = c[0], px[1] = c[1], px[2] = c[2], px[3] = 255;
                    }
                }
            ok = true;
        }
    }
    free(d);
    return ok;
}

static std::vector<uint8_t> read_file(const char *path) {
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize((size_t)n);
        if (fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    fclose(f);
    return v;
}

// The HUD's heart pictures (Pack/Bootup.pack > Layout/Common.sblarc > timg/__Combined.bntx: Nt_HeartNew_00 the whole
// heart, _03/_04/_05 one, two and three quarters; R8G8: red = filled, green = the shape; the game colours them) as one
// strip of white pictures, the HUD's colours given when drawn. Empty: nothing made.
// One BC4 block (8 bytes) into 16 values
static void bc4_block(const uint8_t *k, uint8_t out[16]) {
    unsigned r0 = k[0], r1 = k[1], pal[8] = {r0, r1};
    if (r0 > r1)
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * r0 + i * r1) / 7;
    else {
        for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * r0 + i * r1) / 5;
        pal[6] = 0, pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)k[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)pal[bits >> (3 * i) & 7];
}

static std::vector<uint8_t> load_hearts(std::vector<uint8_t> &stamps, std::vector<uint8_t> &pins) {
    std::vector<uint8_t> out;
    char path[1200];
    snprintf(path, sizeof path, "%s/Pack/Bootup.pack", g_romfs);
    std::vector<uint8_t> pack = read_file(path);
    uint32_t off, lo, hi, off2, lo2, hi2;
    if (pack.empty() || sarc_find(pack.data(), pack.size(), "Layout/Common.sblarc", &off, &lo, &hi) < 0 ||
        (uint64_t)off + hi > pack.size() || lo > hi)
        return out;
    size_t n = 0;
    uint8_t *d = yaz0_unpack(pack.data() + off + lo, hi - lo, &n);
    if (!d) return out;
    if (sarc_find(d, n, "timg/__Combined.bntx", &off2, &lo2, &hi2) >= 0 && (uint64_t)off2 + hi2 <= n && lo2 < hi2) {
        const uint8_t *b = d + off2 + lo2, *end = d + off2 + hi2;
        size_t bn = hi2 - lo2;
        static const char *const names[] = {"Nt_HeartNew_00^f", "Nt_HeartNew_03^f", "Nt_HeartNew_04^f", "Nt_HeartNew_05^f"};
        std::vector<uint8_t> strip((size_t)HEART_PX * 5 * HEART_PX * 4, 0);
        int found = 0;
        for (int k = 0; k < 4; k++) {
            for (const uint8_t *r = find4(b, end, "BRTI"); r && r + 0x78 <= end; r = find4(r + 4, end, "BRTI")) {
                uint64_t no, mp, data;
                memcpy(&no, r + 0x60, 8), memcpy(&mp, r + 0x70, 8);
                if (no + 2 > bn) continue;
                unsigned len = b[no] | b[no + 1] << 8;
                if (no + 2 + len > bn || len != strlen(names[k]) || memcmp(b + no + 2, names[k], len)) continue;
                uint32_t fmt = rd32(r + 0x1c), w = rd32(r + 0x24), h = rd32(r + 0x28), bh = rd32(r + 0x34);
                if (mp + 8 > bn) break;
                memcpy(&data, b + mp, 8);
                uint64_t gob_rows = 8ull << (bh < 6 ? bh : 0), rows = (h + gob_rows - 1) / gob_rows * gob_rows;
                uint64_t size = ((uint64_t)w * 2 + 63) / 64 * 64 * rows;
                if (fmt != 0x0901 || w != HEART_PX || h != HEART_PX || bh >= 6 || data + size > bn) break;
                std::vector<uint8_t> lin((size_t)w * h * 2);
                tegra_deswizzle(b + data, lin.data(), w, h, 1, 2, bh, 0);
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++) {
                        unsigned fill = lin[(y * w + x) * 2], shape = lin[(y * w + x) * 2 + 1];
                        int slot = k == 0 ? 4 : k;  // (_00: the full heart; its shape is slot 0 too)
                        uint8_t *px = &strip[((size_t)y * HEART_PX * 5 + (size_t)slot * HEART_PX + x) * 4];
                        px[0] = px[1] = px[2] = 255, px[3] = (uint8_t)(k == 0 ? shape : fill * shape / 255);
                        if (k == 0) {
                            uint8_t *sh = &strip[((size_t)y * HEART_PX * 5 + x) * 4];
                            sh[0] = sh[1] = sh[2] = 255, sh[3] = (uint8_t)shape;
                        }
                    }
                found++;
                break;
            }
        }
        if (found == 4) out.swap(strip);
        // the stamps: Nt_MapStampIconMarge_00, the 9 stamp pictures in a row (No 27..35: sword, bow, shield, pot, star,
        // chest, skull, leaf, gem), R8G8: red = the picture's light, green = its shape
        for (const uint8_t *r = find4(b, end, "BRTI"); r && r + 0x78 <= end; r = find4(r + 4, end, "BRTI")) {
            static const char name[] = "Nt_MapStampIconMarge_00^f";
            uint64_t no, mp, data;
            memcpy(&no, r + 0x60, 8), memcpy(&mp, r + 0x70, 8);
            if (no + 2 > bn) continue;
            unsigned len = b[no] | b[no + 1] << 8;
            if (no + 2 + len > bn || len != sizeof name - 1 || memcmp(b + no + 2, name, len)) continue;
            uint32_t fmt = rd32(r + 0x1c), w = rd32(r + 0x24), h = rd32(r + 0x28), bh = rd32(r + 0x34);
            if (mp + 8 > bn) break;
            memcpy(&data, b + mp, 8);
            uint64_t gob_rows = 8ull << (bh < 6 ? bh : 0), rows = (h + gob_rows - 1) / gob_rows * gob_rows;
            uint64_t size = ((uint64_t)w * 2 + 63) / 64 * 64 * rows;
            if (fmt != 0x0901 || w != STAMP_PX * 9 || h != STAMP_PX || bh >= 6 || data + size > bn) break;
            std::vector<uint8_t> lin((size_t)w * h * 2);
            tegra_deswizzle(b + data, lin.data(), w, h, 1, 2, bh, 0);
            stamps.assign((size_t)w * h * 4, 0);
            for (size_t k = 0; k < (size_t)w * h; k++) {
                uint8_t *px = &stamps[k * 4];
                px[0] = px[1] = px[2] = lin[k * 2], px[3] = lin[k * 2 + 1];
            }
            break;
        }
        // the map pins: Nt_MapPinNewF_00, Nt_MapPinNew2F_00 .. 5F (No 37..41: one Sheikah glyph each), 64 px BC5: red =
        // the glyph's light, green = its shape; the game draws them in the pin's colour
        static const char *const pin_names[] = {"Nt_MapPinNewF_00^t", "Nt_MapPinNew2F_00^t", "Nt_MapPinNew3F_00^t",
                                                "Nt_MapPinNew4F_00^t", "Nt_MapPinNew5F_00^t"};
        std::vector<uint8_t> pstrip((size_t)PIN_PX * 5 * PIN_PX * 4, 0);
        int pfound = 0;
        for (int k = 0; k < 5; k++) {
            for (const uint8_t *r = find4(b, end, "BRTI"); r && r + 0x78 <= end; r = find4(r + 4, end, "BRTI")) {
                uint64_t no, mp, data;
                memcpy(&no, r + 0x60, 8), memcpy(&mp, r + 0x70, 8);
                if (no + 2 > bn) continue;
                unsigned len = b[no] | b[no + 1] << 8;
                if (no + 2 + len > bn || len != strlen(pin_names[k]) || memcmp(b + no + 2, pin_names[k], len)) continue;
                uint32_t fmt = rd32(r + 0x1c), w = rd32(r + 0x24), h = rd32(r + 0x28), bh = rd32(r + 0x34);
                if (mp + 8 > bn) break;
                memcpy(&data, b + mp, 8);
                uint32_t bw = w / 4, bhk = h / 4;  // (blocks of 4x4, 16 bytes each)
                uint64_t gob_rows = 8ull << (bh < 6 ? bh : 0), rows = (bhk + gob_rows - 1) / gob_rows * gob_rows;
                uint64_t size = ((uint64_t)bw * 16 + 63) / 64 * 64 * rows;
                if (fmt != 0x1e01 || w != PIN_PX || h != PIN_PX || bh >= 6 || data + size > bn) break;
                std::vector<uint8_t> lin((size_t)bw * bhk * 16);
                tegra_deswizzle(b + data, lin.data(), bw, bhk, 1, 16, bh, 0);
                for (uint32_t by = 0; by < bhk; by++)
                    for (uint32_t bx = 0; bx < bw; bx++) {
                        uint8_t lr[16], la[16];
                        bc4_block(&lin[(by * bw + bx) * 16], lr), bc4_block(&lin[(by * bw + bx) * 16 + 8], la);
                        for (int i = 0; i < 16; i++) {
                            uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                            uint8_t *px = &pstrip[((size_t)y * PIN_PX * 5 + (size_t)k * PIN_PX + x) * 4];
                            px[0] = px[1] = px[2] = lr[i], px[3] = la[i];
                        }
                    }
                pfound++;
                break;
            }
        }
        if (pfound == 5) pins.swap(pstrip);
    }
    free(d);
    return out;
}

// (runtime/nvn/vulkan/device.c boot_splash_banner; the main thread) the game files' folder
extern "C" void coop_banner_init(const char *root) {
    const char *off = getenv("SAVAGEWIND_COOP_SPLITMAP");
    if (off && !strcmp(off, "0")) return;
    snprintf(g_romfs, sizeof g_romfs, "%s/romfs", root && *root ? root : "extracted");
    snprintf(g_dir, sizeof g_dir, "%s/UI/MapTex/MainField", g_romfs);
    const char *fog = getenv("SAVAGEWIND_COOP_MAPFOG");
    if (fog && !strcmp(fog, "0")) g_towers = SPLIT_TOWERS_ALL, g_towers_known = true;
    g_on = true;
}

// (the banner's main loop, ~20 a second) makes the map pictures the drawing asked for: decoded here, uploaded with the
// GPU lock held (between the drawing thread's frames, as the boot picture's own are)
extern "C" void coop_banner_tick(void) {
    if (!g_on) return;
    gpu_lock();
    bool need_icons = !g_base_tried;
    g_base_tried = true;
    bool known = g_towers_known;
    uint32_t towers = g_towers;
    int want[16][2], nwant = known ? g_nwant : 0;
    memcpy(want, g_want, sizeof want);
    gpu_unlock();
    if (need_icons) {
        std::vector<uint8_t> stamps, pins, hearts = load_hearts(stamps, pins);
        gpu_lock();
        {
            CtxScope cs;
            if (hearts.size() && cs.ok) g_hearts = vk_imgui_texture_rgba(hearts.data(), HEART_PX * 5, HEART_PX);
            if (stamps.size() && cs.ok) g_stamps = vk_imgui_texture_rgba(stamps.data(), STAMP_PX * 9, STAMP_PX);
            if (pins.size() && cs.ok) g_pins = vk_imgui_texture_rgba(pins.data(), PIN_PX * 5, PIN_PX);
        }
        gpu_unlock();
        LOG("[split] hearts: %s, pins: %s\n", hearts.size() ? "the game's" : "drawn", pins.size() ? "the game's" : "drawn");
        return;
    }
    if (known && towers != g_base_towers) {  // (the whole map: at the start and after a tower is activated)
        g_base_towers = towers;
        int W = MAP_COLS * BASE_PX, H = MAP_ROWS * BASE_PX;
        std::vector<uint8_t> px((size_t)W * H * 4, 0);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        int n = 0;
        uint64_t t0 = SDL_GetTicks();
        for (int c = 0; c < MAP_COLS; c++)
            for (int r = 0; r < MAP_ROWS; r++)
                n += load_square(3, c, r, BASE_PX, &px[((size_t)r * BASE_PX * W + (size_t)c * BASE_PX) * 4], W * 4,
                                 sq_shown(c, r, towers));
        gpu_lock();
        {
            CtxScope cs;
            if (n && cs.ok) {
                if (g_base) vk_imgui_texture_free(g_base);
                g_base = vk_imgui_texture_rgba(px.data(), W, H);
            }
        }
        gpu_unlock();
        LOG("[split] map: %d of %d squares, towers revealed %04x (%u ms)\n", n, MAP_COLS * MAP_ROWS, (unsigned)towers,
            (unsigned)(SDL_GetTicks() - t0));
        return;
    }
    for (int i = 0; i < nwant; i++) {  // (one new square a tick: ~10 ms each)
        bool have = false;
        uint32_t shown = sq_shown(want[i][0], want[i][1], towers);
        gpu_lock();
        for (Detail &d : g_detail) have = have || (d.col == want[i][0] && d.row == want[i][1] && d.shown == shown);
        gpu_unlock();
        if (have) continue;
        std::vector<uint8_t> px((size_t)DETAIL_PX * DETAIL_PX * 4);
        bool ok = load_square(1, want[i][0], want[i][1], DETAIL_PX, px.data(), DETAIL_PX * 4, shown);
        gpu_lock();
        {
            CtxScope cs;  // (ends before the unlock: the context is shared with the drawing thread)
            if (cs.ok) {
                if (g_detail.size() >= DETAIL_MAX) {  // (the least recently drawn goes)
                    size_t old = 0;
                    for (size_t j = 1; j < g_detail.size(); j++)
                        if (g_detail[j].used < g_detail[old].used) old = j;
                    if (g_detail[old].tex) vk_imgui_texture_free(g_detail[old].tex);
                    g_detail.erase(g_detail.begin() + old);
                }
                g_detail.push_back({want[i][0], want[i][1], shown,
                                    ok ? vk_imgui_texture_rgba(px.data(), DETAIL_PX, DETAIL_PX) : 0, g_frame});
            }
        }
        gpu_unlock();
        break;
    }
}

// ---- drawing (the boot picture's thread, GPU lock held)
struct Seen { uint32_t seq; uint64_t changed_ms; bool have, field_known; float field[2], shown[2]; SplitHudPlayer last; };
static Seen g_seen[SPLIT_HUD_PLAYERS];
static float g_cx, g_cz, g_w;  // the view: its centre and width in metres (eased)
static bool g_view_set;
static const ImU32 k_col[SPLIT_HUD_PLAYERS] = {IM_COL32(80, 160, 255, 255), IM_COL32(255, 86, 86, 255),
                                                IM_COL32(90, 220, 110, 255), IM_COL32(255, 214, 70, 255)};

static uint64_t now_ms(void) {
    return SDL_GetTicks();
}

static void text_shadow(ImDrawList *dl, ImFont *f, float size, ImVec2 p, ImU32 col, const char *t) {
    dl->AddText(f, size, ImVec2(p.x + size * 0.06f, p.y + size * 0.06f), IM_COL32(0, 0, 0, 200), t);
    dl->AddText(f, size, p, col, t);
}

// The map and the party panel over the whole window; false: nothing to show yet (the picture stays)
extern "C" bool coop_banner_draw(ImDrawList *dl, ImVec2 ds, float s, ImFont *font, float dt) {
    if (!g_on) return false;
    SplitHud *hud = coop_split_hud();
    if (!hud) return false;
    g_frame++;
    uint64_t now = now_ms();
    SplitHudPlayer p[SPLIT_HUD_PLAYERS];
    bool live[SPLIT_HUD_PLAYERS] = {}, any = false;
    for (int i = 0; i < SPLIT_HUD_PLAYERS; i++) {
        SplitHudPlayer &src = hud->p[i];
        uint32_t a = __atomic_load_n(&src.seq, __ATOMIC_ACQUIRE);
        if (!a || (a & 1)) a = 0;
        else {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            memcpy(&p[i], (const void *)&src, sizeof p[i]);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&src.seq, __ATOMIC_ACQUIRE) != a) a = 0;  // (being written: last copy stays)
        }
        Seen &sn = g_seen[i];
        if (a) {
            p[i].name[sizeof p[i].name - 1] = 0;
            bool sane = true;
            const float *f = p[i].pos;
            for (int k = 0; k < 9; k++) sane = sane && isfinite(f[k]);  // (pos, fwd, life and stamina: one run of floats)
            if (sane) {
                if (a != sn.seq) sn.changed_ms = now;
                sn.seq = a, sn.last = p[i], sn.have = true;
            }
        }
        if (!sn.have || now - sn.changed_ms > STALE_MS) continue;  // (no line yet, or that game is gone)
        p[i] = sn.last;  // (mid-write this frame: the last good copy)
        live[i] = true;
        if (p[i].where == SPLIT_WHERE_FIELD) {
            if (!sn.field_known || fabsf(p[i].pos[0] - sn.shown[0]) + fabsf(p[i].pos[2] - sn.shown[1]) > 200)
                sn.shown[0] = p[i].pos[0], sn.shown[1] = p[i].pos[2];  // (first seen, or a warp: no gliding across the map)
            sn.field_known = true, sn.field[0] = p[i].pos[0], sn.field[1] = p[i].pos[2];
        }
        if (sn.field_known) {  // (the dot glides to the newest position: lines come ~20 a second, frames ~60)
            float k = 1 - expf(-(dt > 0 && dt < 1 ? dt : 1.0f / 60) * 14);
            for (int c = 0; c < 2; c++) sn.shown[c] += (sn.field[c] - sn.shown[c]) * k;
        }
        any = any || p[i].where != SPLIT_WHERE_NONE;
    }
    if (!any) return false;

    // the revealed regions: every player's activated towers together (a player whose towers are not read yet adds none;
    // nobody's read 10 s after the first line: the whole map)
    {
        static uint64_t first_ms;
        if (!first_ms) first_ms = now;
        uint32_t u = 0;
        bool k = false;
        for (int i = 0; i < SPLIT_HUD_PLAYERS; i++)
            if (live[i] && (p[i].towers & SPLIT_TOWERS_KNOWN)) u |= p[i].towers & SPLIT_TOWERS_ALL, k = true;
        if (k) g_towers = u, g_towers_known = true;
        else if (!g_towers_known && now - first_ms > 10000) g_towers = SPLIT_TOWERS_ALL, g_towers_known = true;
    }

    // the layout: the map over the whole window (2026-11-27: map only); the old party panel
    // (hearts, stamina, where) on the left with SAVAGEWIND_COOP_SPLITPANEL=1
    const char *pv = getenv("SAVAGEWIND_COOP_SPLITPANEL");
    bool panel = pv && pv[0] == '1';
    float panel_w = panel ? ds.x * 0.34f : 0;
    ImVec2 m0(panel_w, 0), m1(ds.x, ds.y);
    float mw = m1.x - m0.x, mh = m1.y - m0.y;
    dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(10, 14, 30, 255));

    // the view: everyone's overworld position (a player in a shrine: where they were last seen outside)
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    int n_pts = 0;
    for (int i = 0; i < SPLIT_HUD_PLAYERS; i++) {
        if (!live[i] || !g_seen[i].field_known) continue;
        for (int k = 0; k < 2; k++) lo[k] = fminf(lo[k], g_seen[i].shown[k]), hi[k] = fmaxf(hi[k], g_seen[i].shown[k]);
        n_pts++;
    }
    int zoom = hud->zoom >= 0 && hud->zoom < SPLIT_ZOOM_COUNT ? hud->zoom : SPLIT_ZOOM_NORMAL;
    static const float k_min_w[SPLIT_ZOOM_COUNT] = {350, 900, 2200};
    float tcx = n_pts ? (lo[0] + hi[0]) * 0.5f : 0, tcz = n_pts ? (lo[1] + hi[1]) * 0.5f : 0;
    float aspect = mh / mw;
    float span = n_pts ? fmaxf(hi[0] - lo[0], (hi[1] - lo[1]) / aspect) : 12000;
    float tw = fmaxf(span * 1.5f + 120, k_min_w[zoom]);
    tw = fminf(tw, 13000);
    if (!g_view_set) g_cx = tcx, g_cz = tcz, g_w = tw, g_view_set = true;
    float k = 1 - expf(-(dt > 0 && dt < 1 ? dt : 1.0f / 60) * 2.5f);
    g_cx += (tcx - g_cx) * k, g_cz += (tcz - g_cz) * k;
    g_w = expf(logf(g_w) + (logf(tw) - logf(g_w)) * k);
    float ppm = mw / g_w;  // screen pixels a metre
    float vx0 = g_cx - g_w * 0.5f, vz0 = g_cz - mh / ppm * 0.5f;
    auto to_screen = [&](float x, float z) { return ImVec2(m0.x + (x - vx0) * ppm, m0.y + (z - vz0) * ppm); };

    dl->PushClipRect(m0, m1, true);
    if (g_base) {  // the whole map, then the close squares in view on top of it
        ImVec2 a = to_screen(MAP_X0, MAP_Z0), b = to_screen(MAP_X0 + MAP_COLS * 1000, MAP_Z0 + MAP_ROWS * 1000);
        dl->AddImage((ImTextureID)g_base, a, b);
    }
    g_nwant = 0;
    if (ppm * 1000 > BASE_PX * 2.2f) {
        int c0 = (int)floorf((vx0 - MAP_X0) / 1000), c1 = (int)floorf((vx0 + g_w - MAP_X0) / 1000);
        int r0 = (int)floorf((vz0 - MAP_Z0) / 1000), r1 = (int)floorf((vz0 + mh / ppm - MAP_Z0) / 1000);
        for (int c = c0 < 0 ? 0 : c0; c <= c1 && c < MAP_COLS; c++)
            for (int r = r0 < 0 ? 0 : r0; r <= r1 && r < MAP_ROWS; r++) {
                if (g_nwant < 16) g_want[g_nwant][0] = c, g_want[g_nwant][1] = r, g_nwant++;
                uint32_t shown = sq_shown(c, r, g_towers);
                for (Detail &d : g_detail)
                    if (d.col == c && d.row == r && d.shown == shown) {
                        d.used = g_frame;
                        if (d.tex)
                            dl->AddImage((ImTextureID)d.tex, to_screen(MAP_X0 + c * 1000.0f, MAP_Z0 + r * 1000.0f),
                                         to_screen(MAP_X0 + (c + 1) * 1000.0f, MAP_Z0 + (r + 1) * 1000.0f));
                    }
            }
    }
    // a scale bar
    {
        static const float steps[] = {50, 100, 200, 500, 1000, 2000, 5000};
        float want = mw * 0.22f / ppm, len = steps[0];
        for (float st : steps) if (st <= want) len = st;
        ImVec2 a(m1.x - 20 * s - len * ppm, m1.y - 22 * s), b(m1.x - 20 * s, m1.y - 22 * s);
        dl->AddLine(a, b, IM_COL32(255, 255, 255, 220), 3 * s);
        char t[32];
        snprintf(t, sizeof t, len >= 1000 ? "%.0f km" : "%.0f m", len >= 1000 ? len / 1000 : len);
        text_shadow(dl, font, 20 * s, ImVec2(a.x, a.y - 26 * s), IM_COL32(255, 255, 255, 230), t);
    }
    // the markers on player 1's map: the game's stamp pictures and pin glyphs (pin colours by eye; yellow and blue swapped after the user compared, 2026-11-27)
    {
        SplitHudMark mk[SPLIT_HUD_MARKS];
        static SplitHudMark last[SPLIT_HUD_MARKS];
        static int last_n;
        uint32_t a = __atomic_load_n(&hud->mark_seq, __ATOMIC_ACQUIRE);
        if (a && !(a & 1)) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            int n = hud->mark_n;
            n = n < 0 ? 0 : n > SPLIT_HUD_MARKS ? SPLIT_HUD_MARKS : n;
            memcpy(mk, (const void *)hud->marks, sizeof(SplitHudMark) * (size_t)n);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&hud->mark_seq, __ATOMIC_ACQUIRE) == a) memcpy(last, mk, sizeof(SplitHudMark) * (size_t)n), last_n = n;
        }
        static const ImU32 pin_col[5] = {IM_COL32(232, 58, 58, 255), IM_COL32(60, 140, 255, 255), IM_COL32(250, 196, 40, 255),
                                         IM_COL32(70, 200, 90, 255), IM_COL32(180, 90, 230, 255)};
        float ms = fmaxf(42 * s, 20);  // (the stamp pictures are 32 px)
        for (int k = 0; k < last_n; k++) {
            const SplitHudMark &m = last[k];
            if (!isfinite(m.x) || !isfinite(m.z)) continue;
            ImVec2 c = to_screen(m.x, m.z);
            if (c.x < m0.x - ms || c.x > m1.x + ms || c.y < m0.y - ms || c.y > m1.y + ms) continue;
            if (m.no >= 27 && m.no <= 35) {
                const float u = 1.0f / 9, i0 = (float)(m.no - 27);
                ImVec2 a(c.x - ms * 0.5f, c.y - ms * 0.5f), b(c.x + ms * 0.5f, c.y + ms * 0.5f);
                if (g_stamps) dl->AddImage((ImTextureID)g_stamps, a, b, ImVec2(u * i0, 0), ImVec2(u * (i0 + 1), 1));
                else dl->AddCircleFilled(c, ms * 0.3f, IM_COL32(255, 255, 255, 230));
            } else if (m.no >= 37 && m.no <= 41) {  // (a map pin as the game draws it: its glyph in its colour, a glow
                                                     // behind and a beam of light going up)
                int i0 = m.no - 37;
                ImU32 col = pin_col[i0], glow = (col & 0x00ffffffu) | 0x50000000u, clear = col & 0x00ffffffu;
                float g = ms * 0.9f;
                dl->AddRectFilledMultiColor(ImVec2(c.x - 1.5f * s, c.y - ms * 1.6f), ImVec2(c.x + 1.5f * s, c.y - g * 0.4f), clear,
                                            clear, glow | 0xff000000u, glow | 0xff000000u);
                dl->AddCircleFilled(c, g * 0.5f, (col & 0x00ffffffu) | 0x30000000u);
                dl->AddCircleFilled(c, g * 0.36f, glow);
                if (g_pins) {
                    const float u = 1.0f / 5;
                    ImVec2 a(c.x - g * 0.5f, c.y - g * 0.5f), b(c.x + g * 0.5f, c.y + g * 0.5f);
                    dl->AddImage((ImTextureID)g_pins, ImVec2(a.x + 1.5f * s, a.y + 1.5f * s), ImVec2(b.x + 1.5f * s, b.y + 1.5f * s),
                                 ImVec2(u * i0, 0), ImVec2(u * (i0 + 1), 1), IM_COL32(0, 0, 0, 200));
                    dl->AddImage((ImTextureID)g_pins, a, b, ImVec2(u * i0, 0), ImVec2(u * (i0 + 1), 1), col);
                } else dl->AddCircleFilled(c, g * 0.25f, col);
            }
        }
    }
    // the players: a dot with the way they face, and the name (in a shrine or a Divine Beast: hollow, where they went in)
    for (int i = SPLIT_HUD_PLAYERS - 1; i >= 0; i--) {
        if (!live[i] || !g_seen[i].field_known) continue;
        bool out = p[i].where != SPLIT_WHERE_FIELD;
        ImVec2 c = to_screen(g_seen[i].shown[0], g_seen[i].shown[1]);
        float r = 11 * s;
        if (!out) {
            float fx = p[i].fwd[0], fz = p[i].fwd[1], l = sqrtf(fx * fx + fz * fz);
            if (l > 1e-3f) {
                fx /= l, fz /= l;
                ImVec2 tip(c.x + fx * r * 2.1f, c.y + fz * r * 2.1f);
                ImVec2 l1(c.x - fz * r * 0.8f, c.y + fx * r * 0.8f), r1(c.x + fz * r * 0.8f, c.y - fx * r * 0.8f);
                dl->AddTriangleFilled(tip, l1, r1, IM_COL32(255, 255, 255, 230));
            }
            dl->AddCircleFilled(c, r + 3 * s, IM_COL32(0, 0, 0, 200));
            dl->AddCircleFilled(c, r, k_col[i]);
        } else {
            dl->AddCircle(c, r, IM_COL32(0, 0, 0, 200), 0, 6 * s);
            dl->AddCircle(c, r, k_col[i], 0, 3 * s);
        }
        float fs = 22 * s;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, p[i].name);
        text_shadow(dl, font, fs, ImVec2(c.x - ts.x * 0.5f, c.y + r + 6 * s), IM_COL32(255, 255, 255, 240), p[i].name);
    }
    dl->PopClipRect();

    // the party panel
    if (!panel) return true;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(panel_w, ds.y), IM_COL32(8, 12, 28, 235));
    dl->AddLine(ImVec2(panel_w, 0), ImVec2(panel_w, ds.y), IM_COL32(255, 255, 255, 40), 2 * s);
    int n_live = 0;
    for (int i = 0; i < SPLIT_HUD_PLAYERS; i++) n_live += live[i];
    float pad = 16 * s, row_h = (ds.y - pad) / (n_live > 3 ? n_live : 3);
    float y = pad * 0.5f;
    float name_fs = fminf(30 * s, row_h * 0.26f), small_fs = name_fs * 0.78f;
    for (int i = 0; i < SPLIT_HUD_PLAYERS; i++) {
        if (!live[i]) continue;
        float x = pad, w = panel_w - pad * 2;
        dl->AddRectFilled(ImVec2(x - pad * 0.4f, y + name_fs * 0.15f), ImVec2(x - pad * 0.4f + 5 * s, y + row_h - pad),
                          k_col[i]);
        x += 6 * s, w -= 6 * s;
        text_shadow(dl, font, name_fs, ImVec2(x, y), k_col[i], p[i].name);
        float yy = y + name_fs * 1.15f;
        if (p[i].where == SPLIT_WHERE_NONE) {
            text_shadow(dl, font, small_fs, ImVec2(x, yy), IM_COL32(200, 200, 210, 230), tr("Loading"));
            y += row_h;
            continue;
        }
        // hearts as the game's HUD shows them (10 a row; yellow ones beyond the maximum), the stamina wheel beside them
        int quarters = (int)(p[i].life + 0.5f), max_hearts = (int)(p[i].life_max / 4 + 0.5f);
        int total = max_hearts > (quarters + 3) / 4 ? max_hearts : (quarters + 3) / 4;
        total = total < 1 ? 1 : total > 40 ? 40 : total;
        int heart_rows = (total + 9) / 10;
        float wheel_r = fminf(row_h * 0.19f, w * 0.1f);
        float hearts_w = w - wheel_r * 2.6f;
        float avail_h = row_h - pad - name_fs * 1.15f - small_fs * 1.25f;
        int per_row = total < 10 ? total : 10;  // (few hearts: bigger, as on a narrow panel 10 would be tiny)
        float hs = fminf(hearts_w / (per_row > 6 ? per_row : 6), avail_h / heart_rows);
        hs = fminf(hs, name_fs * 1.25f);
        for (int h = 0; h < total; h++) {
            ImVec2 a(x + (h % 10) * hs, yy + (h / 10) * hs), b(a.x + hs * 1.08f, a.y + hs * 1.08f);
            int q = quarters - h * 4;
            q = q < 0 ? 0 : q > 4 ? 4 : q;
            bool extra = h >= max_hearts;  // (yellow: extra hearts from food)
            ImU32 fill = extra ? IM_COL32(255, 214, 60, 255) : IM_COL32(236, 44, 58, 255);
            if (g_hearts) {
                const float u = 1.0f / 5;
                ImVec2 o(hs * 0.05f, hs * 0.07f);
                if (!extra || q)
                    dl->AddImage((ImTextureID)g_hearts, ImVec2(a.x + o.x, a.y + o.y), ImVec2(b.x + o.x, b.y + o.y),
                                 ImVec2(0, 0), ImVec2(u, 1), IM_COL32(0, 0, 0, 140));
                if (!extra) dl->AddImage((ImTextureID)g_hearts, a, b, ImVec2(0, 0), ImVec2(u, 1), IM_COL32(70, 70, 76, 200));
                if (q) dl->AddImage((ImTextureID)g_hearts, a, b, ImVec2(u * q, 0), ImVec2(u * (q + 1), 1), fill);
            } else {  // (no game pictures: round ones)
                ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                dl->AddCircleFilled(c, hs * 0.4f, IM_COL32(70, 70, 76, 200));
                if (q) dl->AddCircleFilled(c, hs * 0.4f * q / 4, fill);
            }
        }
        {
            ImVec2 c(x + w - wheel_r * 1.15f, y + row_h * 0.5f - pad * 0.3f);
            int wheels = (int)ceilf(p[i].stamina_max / 1000 - 0.01f);
            wheels = wheels < 1 ? 1 : wheels > 3 ? 3 : wheels;
            float st = p[i].stamina;
            const ImU32 green = IM_COL32(120, 226, 64, 255), dark = IM_COL32(16, 28, 16, 190);
            const float top = -3.14159265f * 0.5f;
            // the first wheel: a pie (one shape: pieces would show seams)
            float r0 = wheel_r * (wheels > 1 ? 0.62f : 0.9f);
            dl->AddCircleFilled(c, r0 + 2 * s, dark);
            float f = fminf(fmaxf(st / 1000, 0), 1);
            if (f >= 0.999f) dl->AddCircleFilled(c, r0, green);
            else if (f > 0) {
                dl->PathLineTo(c);
                dl->PathArcTo(c, r0, top, top + f * 2 * 3.14159265f);
                dl->PathFillConcave(green);
            }
            // the extra wheels: rings around it
            float ring_w = wheels > 1 ? (wheel_r - r0) / (wheels - 1) : 0;
            for (int k = 1; k < wheels; k++) {
                float rr = r0 + ring_w * (k - 0.5f) + 1 * s, fk = fminf(fmaxf((st - 1000 * k) / 1000, 0), 1);
                dl->AddCircle(c, rr, dark, 0, ring_w);
                if (fk > 0) {
                    dl->PathArcTo(c, rr, top, top + fk * 2 * 3.14159265f);
                    dl->PathStroke(green, 0, ring_w * 0.72f);
                }
            }
        }
        yy += heart_rows * hs + small_fs * 0.25f;
        char t[96];
        // where, when not on the overworld (there the map shows it)
        t[0] = 0;
        if (p[i].where != SPLIT_WHERE_FIELD)
            snprintf(t, sizeof t, "%s", tr(p[i].where == SPLIT_WHERE_SHRINE  ? "In a shrine"
                                           : p[i].where == SPLIT_WHERE_BEAST ? "In a Divine Beast"
                                                                              : "Elsewhere"));
        if (t[0]) text_shadow(dl, font, small_fs, ImVec2(x, yy), IM_COL32(215, 220, 235, 235), t);
        y += row_h;
    }
    return true;
}
#else
extern "C" void coop_banner_init(const char *root) { (void)root; }
extern "C" void coop_banner_tick(void) {}
extern "C" bool coop_banner_draw(ImDrawList *, ImVec2, float, ImFont *, float) { return false; }
#endif
