// The PSP's picture, drawn by the GE: the background tile layers as a
// texture, the game's polygons over them, then the front tile layers (their
// empty pixels see-through), scaled to the screen's 272 lines and centred
// (351 x 272 of the 480 x 272 screen). The runtime is in external 3D mode
// (Video::set_external_3d), as on the Dreamcast (platform/dreamcast/game/
// renderer.h, which this follows): it hands over this frame's polygons and
// both tile layers, composed straight into the GE's 16-bit formats
// (M2_DC_SPEED with M2_PSP_BGR: background RGB565, front ARGB1555 with 0
// see-through), and draws no 3D itself.
//
// Order: as the CPU reference and platform/vita/polygon_order.h, descending
// window, ascending z, newest first on ties, drawn back to front with the
// depth test off. Polygons go through the GE's transform with an identity
// view and a projection that only moves the depth into w, so texture
// coordinates are interpolated with perspective (the GE's 2D mode is affine).
//
// Textures, as the Dreamcast's: a source is a texture's 4-bit texel indices
// (read_texel_quad from the board's texture RAM), at most kTextureLimit a
// side (larger ones sampled down), kept swizzled as a GU_PSM_T4 texture in a
// cache the frontend gives (the half of each texture sheet the game's writes
// do not reach). The GE has no mirrored repeat: a source with a mirror bit is
// built twice as wide (or high), its second half mirrored, which repeats the
// same.
//
// Colour, as the CPU rasterizer (raster.cpp, draw_tex_span) computes it: a
// texel's luma from luma RAM at the polygon's luma base, times the polygon's
// light, through the colour translation table for each channel of the
// polygon's colour. That is one 16-entry palette (32-bit) per colour, light,
// luma base and transparency, built each frame; the GE takes any number, and
// the vertex colour is white. (The Dreamcast, with 64 palette banks, keeps
// the texels' luma as greys times a vertex colour; the translation table is
// not linear, and textures whose colour changes with the luma, such as the
// windows, came out grey here that way.) A translucent material's texel 15
// is see-through. A checkered polygon (texheader bit 15: the CPU draws every
// other pixel, the fences) is drawn at half alpha: the GE has no
// checkerboard mask, and from a distance it is the same.
//
// The GE draws while the CPU runs the next frames: draw hands it the display
// list and returns; finish() waits for it and shows the frame. The GE reads
// the layers, textures and palettes from main RAM, so finish() comes first
// in the next draw (which rewrites the textures, palettes and list) and,
// from the frontend, before the frame that composes the layers again.
#pragma once

#include "runtime/raster_texel.h"

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <psprtc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace psp {

inline uint64_t now_us() {
    uint64_t tick = 0;
    sceRtcGetCurrentTick(&tick); // microseconds on the PSP
    return tick;
}

class Renderer {
public:
    struct Region { uint8_t *base; size_t bytes; }; // main RAM for the texture cache (16-byte aligned)

    explicit Renderer(std::vector<Region> cache) : regions_(std::move(cache)) {
        for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
        list_ = static_cast<uint32_t *>(aligned(kListBytes));
        palettes_ = static_cast<uint32_t *>(aligned(kPaletteBytes));
        vertices_ = static_cast<Vertex *>(aligned(kVertices * sizeof(Vertex)));
        if (!list_ || !palettes_ || !vertices_) throw std::runtime_error("no RAM for the display list");

        sceGuInit();
        sceGuStart(GU_DIRECT, list_);
        sceGuDrawBuffer(GU_PSM_5650, reinterpret_cast<void *>(kFrame0), kStride);
        sceGuDispBuffer(kScreenW, kScreenH, reinterpret_cast<void *>(kFrame1), kStride);
        sceGuDepthBuffer(reinterpret_cast<void *>(kDepth), kStride); // (never written: depth writes off)
        sceGuOffset(2048 - kScreenW / 2, 2048 - kScreenH / 2);
        sceGuViewport(2048, 2048, kScreenW, kScreenH);
        sceGuDepthRange(65535, 0);
        sceGuScissor(int(kOffsetX), 0, int(kW * kScale + 0.5f), kScreenH); // (x, y, width, height)
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        sceGuDisable(GU_CULL_FACE); // the game's own facing decides
        sceGuShadeModel(GU_SMOOTH);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        sceDisplayWaitVblankStart();
        sceGuDisplay(GU_TRUE);
    }
    ~Renderer() {
        finish();
        sceGuTerm();
    }
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // video: after GameLoop::run_frame, in external 3D mode.
    void draw(rt::Video &video) {
        finish(); // the last frame (its display list and palettes are about to be rewritten)
        uint64_t t = now_us();
        auto lap = [&](int step) {
            const uint64_t now = now_us();
            step_us[step] += now - t;
            t = now;
        };
        // The layers are read by the GE from main RAM: out of the CPU's cache.
        const std::vector<uint16_t> &back = video.background16(), &front = video.foreground16();
        if (video.background16_generation() != back_flushed_) {
            sceKernelDcacheWritebackRange(back.data(), unsigned(back.size() * 2));
            back_flushed_ = video.background16_generation();
        }
        if (video.foreground16_generation() != front_flushed_) {
            sceKernelDcacheWritebackRange(front.data(), unsigned(front.size() * 2));
            front_flushed_ = video.foreground16_generation();
        }
        const bool scrolled = video.back_scrolled();
        if (scrolled && !update_scroll(video)) throw std::runtime_error("no RAM for the scrolled layer");
        lap(0);
        const auto &polys = video.gpu_polys();
        const rt::VideoMem &mem = video.gpu_mem();
        sort(polys);
        if (flush_) { // the texture cache filled up last frame
            sources_.clear();
            region_ = 0;
            region_used_ = 0;
            flush_ = false;
            ++flushes;
        }
        ++colour_stamp_; // (palette, translation and luma RAM may have changed since the last frame)
        palette_index_.clear();
        palettes_used_ = 0;
        builds_ = 0;

        // Textures and palettes front to back first: the build budget goes to
        // what is nearest (drawn last, largest on screen).
        no_build = no_palette = 0;
        for (Entry &e : order_) {
            const rt::GeoPoly &poly = polys[e.index];
            if (poly.window > video.gpu_windows() || poly.num_vertices < 3 || poly.num_vertices > 8) continue;
            if (!((poly.texheader[0] >> 14) & 1)) continue;
            e.palette = palette_for(poly, mem);
            if (!e.palette) { ++no_palette; continue; }
            e.source = source_for(poly, mem);
            if (!e.source) { ++no_build; e.palette = nullptr; }
        }
        if (palettes_used_) sceKernelDcacheWritebackRange(palettes_, palettes_used_ * 64);
        lap(1);

        // The list is built whole and handed over at the end (GU_SEND): in
        // GU_DIRECT every sceGuDrawArray and sceGuGetMemory updates the GE's
        // stall address, a system call, two a polygon.
        sceGuStart(GU_SEND, list_);
        // (GU_DIRECT's sceGuStart sets the draw buffer, which sceGuSwapBuffers
        // swaps; GU_SEND's does not: without this the GE drew into one buffer.)
        sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void *>(draw_buffer_), kStride);
        sceGuClearColor(0xff000000u);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuTexFlush(); // textures built this frame may sit where others were
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        // The background layers, opaque; or, with the scrolled layer, that
        // layer opaque and the others (ARGB1555, see-through) over it.
        sceGuEnable(GU_TEXTURE_2D);
        sceGuDisable(GU_BLEND);
        sceGuDisable(GU_ALPHA_TEST);
        if (scrolled) {
            scroll_layer(video.scroll_x(), video.scroll_y());
            sceGuEnable(GU_BLEND);
            sceGuEnable(GU_ALPHA_TEST);
            layer(back.data(), GU_PSM_5551);
        } else {
            layer(back.data(), GU_PSM_5650);
            sceGuEnable(GU_BLEND);
            sceGuEnable(GU_ALPHA_TEST);
        }

        // The polygons, back to front.
        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadMatrix(&kProjection);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        sceGumUpdateMatrix(); // (the polygons use sceGuDrawArray: the matrices are in place)
        drawn = textured = skipped = checkered = 0;
        const Source *source_now = nullptr;
        const uint32_t *palette_now = nullptr;
        bool texturing = true; // as layer() left it
        size_t used = 0;
        vertices_used_ = 0;
        for (auto e = order_.rbegin(); e != order_.rend(); ++e) {
            const rt::GeoPoly &poly = polys[e->index];
            if (poly.window > video.gpu_windows() || poly.num_vertices < 3 || poly.num_vertices > 8) continue;
            const int renderer = (poly.texheader[0] >> 13) & 3;
            const Source *source = nullptr;
            if (renderer & 2) source = e->palette ? e->source : nullptr; // (found in the pass above)
            else if (renderer & 1) continue; // as the Vita and the Dreamcast: not drawn
            constexpr size_t bytes = 64; // (the commands; the vertices are in vertices_)
            if (used + bytes > kListBytes - kListReserve || vertices_used_ + poly.num_vertices > kVertices) {
                ++skipped;
                continue;
            }
            if (source) {
                if (!texturing) {
                    sceGuEnable(GU_TEXTURE_2D);
                    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
                    texturing = true;
                    source_now = nullptr;
                }
                if (source != source_now) {
                    if (!source_now) { // (the same for every source; the layers had their own)
                        sceGuTexMode(GU_PSM_T4, 0, 0, GU_TRUE);
                        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
                    }
                    sceGuTexImage(0, int(source->w), int(source->h), int(source->w), source->texels);
                    source_now = source;
                    used += 32;
                }
                if (e->palette != palette_now) {
                    sceGuClutLoad(2, e->palette); // 16 entries: 2 blocks of 8
                    palette_now = e->palette;
                    used += 16;
                }
            } else if (texturing) {
                sceGuDisable(GU_TEXTURE_2D);
                texturing = false;
            }
            // Textured: the palette has the colour (white vertices); flat: the
            // polygon's colour. A checkered polygon at half alpha.
            uint32_t colour = source ? 0xffffffffu : solid(poly, mem);
            if ((poly.texheader[0] >> 15) & 1) {
                colour = (colour & 0x00ffffffu) | 0x80000000u;
                ++checkered;
            }
            if (polygon(poly, video, colour, source)) {
                ++drawn;
                if (source) ++textured;
                used += bytes;
            }
        }
        // The front layers, see-through where they have no pixel.
        sceGuEnable(GU_TEXTURE_2D);
        layer(front.data(), GU_PSM_5551);
        list_bytes = used + vertices_used_ * sizeof(Vertex);
        sceKernelDcacheWritebackRange(vertices_, unsigned(vertices_used_ * sizeof(Vertex)));
        sceGuFinish();
        sceGuSendList(GU_TAIL, list_, &ge_context_);
        if (dump_path) {
            sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
            dump(dump_path);
        }
        pending_ = true; // the GE draws on while the CPU runs the next frames: finish() shows it
        lap(2);
    }
    // Wait for the GE to finish the frame draw() handed it, and show it.
    // Called by draw itself and, by the frontend, before a frame that
    // composes the tile layers again (the GE may still be reading them).
    void finish() {
        if (!pending_) return;
        const uint64_t t = now_us();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        if (vsync) sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        draw_buffer_ = draw_buffer_ == kFrame0 ? kFrame1 : kFrame0;
        pending_ = false;
        step_us[3] += now_us() - t;
    }
    // A picture around the game (overlay.bin: overlay_bytes() of RGB565 as the
    // frame buffers hold it, rows of kScreenW; 16-byte aligned), put into
    // both frame buffers once: the frames clear and draw only inside the
    // scissor, so the sides keep it at no cost a frame. By the GE's block
    // transfer, as dump(): PPSSPP keeps the frame buffers on the PC's GPU.
    static constexpr size_t overlay_bytes() { return size_t(kScreenW) * kScreenH * 2; }
    void set_overlay(const uint16_t *pixels) {
        finish();
        sceKernelDcacheWritebackRange(pixels, unsigned(overlay_bytes()));
        sceGuStart(GU_DIRECT, list_);
        for (uintptr_t frame : {kFrame0, kFrame1})
            sceGuCopyImage(GU_PSM_5650, 0, 0, kScreenW, kScreenH, kScreenW, const_cast<uint16_t *>(pixels), 0, 0,
                           kStride, reinterpret_cast<void *>(0x04000000u + frame));
        sceGuTexSync();
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    }
    // When set, draw writes the frame it drew to this file: the frame buffer
    // as it is (RGB565 with red low, 512 pixels a row, 272 rows;
    // scripts/shot2png.py converts it), to compare with the desktop's
    // m2run --dump at the same frame.
    const char *dump_path = nullptr;
    // Microseconds so far: the layers' cache writeback, sorting and textures
    // and palettes, building the display list (draw); waiting for the GE to
    // finish (finish, with the vertical blank if vsync).
    uint64_t step_us[4] = {};
    // The last frame: polygons drawn (of them textured), left out (display
    // list full), checkered (at half alpha); textured polygons drawn flat
    // (past the build budget, out of palettes); palettes built; texture cache
    // flushes so far; the display list's bytes.
    unsigned drawn = 0, textured = 0, skipped = 0, checkered = 0, flushes = 0, rebuilds = 0;
    unsigned no_build = 0, no_palette = 0;
    unsigned palettes() const { return palettes_used_; }
    size_t list_bytes = 0;
    size_t sources() const { return sources_.size(); }
    bool vsync = false; // wait for the vertical blank before showing a frame

private:
    // VRAM (the GE's addresses are offsets from its base): two 16-bit frame
    // buffers 512 pixels wide, and the depth buffer's place after them.
    static constexpr int kScreenW = 480, kScreenH = 272, kStride = 512;
    static constexpr uintptr_t kFrame0 = 0, kFrame1 = kStride * kScreenH * 2, kDepth = 2 * kFrame1;
    static constexpr int kW = 496, kH = 384, kTexW = 512, kTexH = 512;
    static constexpr float kScale = float(kScreenH) / kH, kOffsetX = (kScreenW - kW * kScale) * 0.5f;
    static constexpr size_t kListBytes = 512 * 1024;   // commands (the busiest frame: ~2,200 polygons, 64 bytes each)
    static constexpr size_t kVertices = 24576;          // a frame's polygon vertices (vertices_)
    static constexpr size_t kListReserve = 16 * 1024;   // the layers' strips and the list's own commands
    static constexpr uint32_t kTextureLimit = 256;      // source textures at most this a side (before mirroring)
    static constexpr unsigned kBuildsPerFrame = 32;     // new source textures a frame
    static constexpr unsigned kPalettes = 4096;         // palettes a frame (16 x 32 bits, 64 bytes each)
    static constexpr size_t kPaletteBytes = kPalettes * 64;
    static_assert(rt::Video::kLayerStride == kTexW, "a layer's rows are the texture's");

    struct Vertex { float u, v; uint32_t abgr; float x, y, z; }; // GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
    struct Sprite { float u, v, x, y, z; };                      // GU_TEXTURE_32BITF | GU_VERTEX_32BITF (2D)
    // w, h: the texture (two periods along a mirrored axis); period_w,
    // period_h: one period of the source as built (sampled down past
    // kTextureLimit); source_w, source_h: the game's size.
    // u_scale, v_scale: the geometrizer's texture coordinates (1/8 texel) to
    // the GE's (0..1 over the texture; a mirrored texture holds two periods).
    // built, chunk_lo..chunk_hi, checked: texture RAM's generation when it was
    // built, the 1 KB chunks it was read from (VideoMem::tex_chunks), and the
    // generation it was last checked against.
    struct Source {
        uint8_t *texels;
        uint32_t w, h, period_w, period_h, source_w, source_h;
        float u_scale, v_scale;
        uint32_t built, chunk_lo, chunk_hi, checked;
    };
    // (source, palette: set by the first pass, for textured polygons)
    struct Entry { uint32_t key; size_t index; const Source *source; const uint32_t *palette; };

    // Clip x and y are the vertex's own (the frontend puts the screen
    // position times w there), clip w its depth; clip z 0 (no depth test).
    // Column-major, as ScePspFMatrix4: the x, y, z and w columns.
    static constexpr ScePspFMatrix4 kProjection = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 0}};

    static void *aligned(size_t bytes) { return ::operator new(bytes, std::align_val_t(64), std::nothrow); }

    uintptr_t draw_buffer_ = kFrame0; // the buffer the GE draws into (the other is shown)
    bool pending_ = false;            // a frame handed to the GE, not yet shown
    uint16_t *dump_buffer_ = nullptr;
    // The frame buffer copied to main RAM by the GE (a block transfer) and
    // written out. Not read from VRAM directly: PPSSPP draws on the PC's GPU
    // and VRAM holds nothing (the first dumps were black); a block transfer
    // from a frame buffer makes it read the picture back.
    void dump(const char *path) {
        constexpr size_t kBytes = size_t(kStride) * kScreenH * 2;
        if (!dump_buffer_) dump_buffer_ = static_cast<uint16_t *>(aligned(kBytes));
        if (!dump_buffer_) return;
        sceGuStart(GU_DIRECT, list_);
        sceGuCopyImage(GU_PSM_5650, 0, 0, kScreenW, kScreenH, kStride,
                       reinterpret_cast<void *>(0x04000000u + draw_buffer_), 0, 0, kStride, dump_buffer_);
        sceGuTexSync();
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        sceKernelDcacheInvalidateRange(dump_buffer_, unsigned(kBytes));
        const SceUID f = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (f < 0) return;
        sceIoWrite(f, dump_buffer_, int(kBytes));
        sceIoClose(f);
    }
    static uint16_t le16(const uint8_t *p, uint32_t index) { return uint16_t(p[index * 2] | p[index * 2 + 1] << 8); }
    static uint32_t abgr(uint32_t argb) {
        return (argb & 0xff00ff00u) | (argb >> 16 & 0xff) | (argb & 0xff) << 16;
    }

    // A layer (rows of kTexW, the first kH shown) over the game's area, in
    // vertical strips of 64 texels: the GE's texture cache works on small
    // blocks, and one 496-wide sprite thrashes it.
    void layer(const uint16_t *pixels, int format) {
        sceGuTexMode(format, 0, 0, GU_FALSE);
        sceGuTexImage(0, kTexW, kTexH, kTexW, pixels);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        constexpr int kStrip = 64, kStrips = (kW + kStrip - 1) / kStrip;
        auto *v = static_cast<Sprite *>(sceGuGetMemory(sizeof(Sprite) * 2 * kStrips));
        for (int s = 0; s < kStrips; ++s) {
            const float u0 = float(s * kStrip), u1 = float(std::min(kW, (s + 1) * kStrip));
            v[s * 2] = {u0, 0.0f, kOffsetX + u0 * kScale, 0.0f, 0.0f};
            v[s * 2 + 1] = {u1, float(kH), kOffsetX + u1 * kScale, float(kScreenH), 0.0f};
        }
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2 * kStrips, nullptr, v);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT); // the polygons' (the CPU masks texture coordinates)
    }

    // The scrolled layer (rt::Video::back_scrolled, the race's scenery: the
    // Dreamcast's renderer does the same): pixmap layer 2's whole 512 x 512
    // as an RGB565 texture in main RAM, the tiles whose pixels changed (all
    // after a pen change) converted by the runtime (scroll_pixel). After
    // finish(): the GE is not reading it. False when there is no RAM for it.
    bool update_scroll(rt::Video &video) {
        if (!scroll_) {
            scroll_ = static_cast<uint16_t *>(aligned(512 * 512 * 2));
            if (!scroll_) return false;
            scroll_all_ = true;
        }
        const bool all = video.scroll_all_changed() || scroll_all_;
        const uint8_t *changed = video.scroll_tiles_changed();
        uint32_t lo = 64, hi = 0; // tile rows written
        for (uint32_t t = 0; t < 64 * 64; ++t) {
            if (!all && !changed[t]) continue;
            const uint32_t tx = (t & 63) * 8, ty = (t >> 6) * 8;
            for (uint32_t y = ty; y < ty + 8; ++y) {
                uint16_t *row = scroll_ + y * 512 + tx;
                for (uint32_t x = 0; x < 8; ++x) row[x] = video.scroll_pixel(tx + x, y);
            }
            lo = std::min(lo, t >> 6);
            hi = std::max(hi, t >> 6);
            ++scroll_tiles;
        }
        if (lo <= hi) sceKernelDcacheWritebackRange(scroll_ + lo * 8 * 512, (hi - lo + 1) * 8 * 512 * 2);
        video.scroll_taken();
        scroll_all_ = false;
        return true;
    }
public:
    unsigned scroll_tiles = 0; // scrolled-layer tiles converted, since the start
private:
    // The game's area from (sx, sy) of the scrolled layer, wrapping as the
    // pixmap does: sprites cut where the texture wraps (so no texture
    // coordinate leaves 0..512), in strips of 64 texels across.
    void scroll_layer(int sx, int sy) {
        sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
        sceGuTexImage(0, 512, 512, 512, scroll_);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        constexpr int kStrip = 64, kMaxPieces = 2 * ((kW + kStrip - 1) / kStrip + 1);
        auto *v = static_cast<Sprite *>(sceGuGetMemory(sizeof(Sprite) * 2 * kMaxPieces));
        int n = 0;
        const int v0 = sy & 511;
        const int ycut = std::min(kH, 512 - v0); // screen rows before the texture wraps
        const int rows[2][3] = {{0, ycut, v0}, {ycut, kH, 0}}; // screen y0, y1, texture v at y0
        for (const auto &r : rows) {
            if (r[0] >= r[1]) continue;
            for (int x0 = 0; x0 < kW;) {
                const int u0 = (sx + x0) & 511;
                const int x1 = std::min({kW, (x0 / kStrip + 1) * kStrip, x0 + (512 - u0)});
                v[n * 2] = {float(u0), float(r[2]), kOffsetX + x0 * kScale, r[0] * kScale, 0.0f};
                v[n * 2 + 1] = {float(u0 + x1 - x0), float(r[2] + r[1] - r[0]), kOffsetX + x1 * kScale, r[1] * kScale,
                                0.0f};
                ++n;
                x0 = x1;
            }
        }
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2 * n, nullptr, v);
    }
    uint16_t *scroll_ = nullptr;
    bool scroll_all_ = false; // the texture is new: every tile

    void sort(const std::vector<rt::GeoPoly> &polys) {
        const size_t count = polys.size();
        order_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t index = count - 1 - i;
            order_[i] = {((255u - uint32_t(polys[index].window)) << 16) | uint32_t(polys[index].z), index, nullptr,
                         nullptr};
        }
        // Key ascending, equal keys index descending (as built): a stable
        // radix sort of the 24-bit key, a byte a pass.
        order_tmp_.resize(count);
        for (unsigned shift = 0; shift < 24; shift += 8) {
            size_t at[257] = {};
            for (const Entry &e : order_) ++at[((e.key >> shift) & 0xff) + 1];
            for (int b = 0; b < 256; ++b) at[b + 1] += at[b];
            for (const Entry &e : order_) order_tmp_[at[(e.key >> shift) & 0xff]++] = e;
            order_.swap(order_tmp_);
        }
    }

    // A colour through the translation table at a luma (0..0x3f), as the
    // CPU rasterizer: ABGR, the GE's order.
    uint32_t translate(uint32_t color, uint32_t luma, const rt::VideoMem &mem) const {
        const uint32_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
        const uint32_t g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
        const uint32_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
        return 0xff000000u | b << 16 | g << 8 | r;
    }
    static uint32_t poly_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        return le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000) & 0x7fff;
    }
    // A flat polygon's colour (raster.cpp's draw_scanline_solid: luma >> 2),
    // through a cache for this frame.
    uint32_t solid(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint32_t key = ((poly.texheader[3] >> 6) & 0x3ff) << 6 | poly.luma >> 2;
        ColourSlot &slot = colours_[(key ^ key >> 7) & (kColourSlots - 1)];
        if (slot.stamp != colour_stamp_ || slot.key != key)
            slot = {colour_stamp_, key, translate(poly_color(poly, mem), poly.luma >> 2, mem)};
        return slot.abgr;
    }
    struct ColourSlot { uint32_t stamp, key, abgr; };
    static constexpr uint32_t kColourSlots = 1024;
    ColourSlot colours_[kColourSlots] = {};
    uint32_t colour_stamp_ = 1; // moves once a drawn frame (the palette slots' too)

    // This frame's palette for a textured polygon: its colour, light, luma
    // base and transparency. Entry t is texel t as draw_tex_span shades it
    // (luma RAM at lumabase + t * 8, times the light / 256, at most 0x3f,
    // through the translation table); texel 15 of a translucent material is
    // see-through. A direct-mapped cache in front of the map.
    const uint32_t *palette_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint32_t color = poly_color(poly, mem), lumabase = poly.texheader[1] & 0xff;
        const bool translucent = (poly.texheader[0] >> 13) & 1;
        const uint32_t key = color | uint32_t(poly.luma) << 15 | lumabase << 23 | uint32_t(translucent) << 31;
        PaletteSlot &slot = palette_slots_[(key ^ key >> 11 ^ key >> 21) & (kPaletteSlots - 1)];
        if (slot.stamp == colour_stamp_ && slot.key == key) return slot.palette;
        auto found = palette_index_.find(key);
        if (found != palette_index_.end() && found->second.stamp == colour_stamp_) {
            slot = found->second;
            return slot.palette;
        }
        if (palettes_used_ >= kPalettes) return nullptr;
        uint32_t *p = palettes_ + palettes_used_++ * 16;
        for (unsigned t = 0; t < 16; ++t) {
            const uint32_t luma =
                std::min<uint32_t>(uint32_t(mem.lumaram[((lumabase << 7) + (t << 3)) * 4]) * poly.luma / 256, 0x3f);
            p[t] = translate(color, luma, mem);
        }
        if (translucent) p[15] = 0;
        const PaletteSlot made{key, colour_stamp_, p};
        palette_index_[key] = made;
        slot = made;
        return p;
    }
    struct PaletteSlot { uint32_t key, stamp; const uint32_t *palette; };
    static constexpr uint32_t kPaletteSlots = 1024;
    PaletteSlot palette_slots_[kPaletteSlots] = {};

    // Main RAM from the cache regions (bump allocation; a full cache is
    // emptied at the start of the next frame).
    uint8_t *allocate(size_t bytes) {
        bytes = (bytes + 63) & ~size_t(63);
        while (region_ < regions_.size()) {
            if (region_used_ + bytes <= regions_[region_].bytes) {
                uint8_t *p = regions_[region_].base + region_used_;
                region_used_ += bytes;
                return p;
            }
            ++region_;
            region_used_ = 0;
        }
        flush_ = true;
        return nullptr;
    }

    // The GE's swizzled layout for a 4-bit texture: blocks of 16 bytes (32
    // texels) by 8 rows, row-major within a block, blocks left to right.
    static size_t swizzled(uint32_t x, uint32_t y, uint32_t row_bytes) {
        const uint32_t xb = x >> 1;
        return size_t((y >> 3) * (row_bytes >> 4) + (xb >> 4)) * 128 + (y & 7) * 16 + (xb & 15);
    }

    // Whether texture RAM the source was read from has been written since it
    // was built (checked once per texture RAM generation).
    static bool stale(Source &s, const rt::VideoMem &mem) {
        const uint32_t now = uint32_t(mem.tex_generation);
        if (s.checked == now) return false;
        s.checked = now;
        for (uint32_t c = s.chunk_lo; c <= s.chunk_hi; ++c)
            if (int32_t(mem.tex_chunks[c] - s.built) > 0) return true;
        return false;
    }

    // A texture's source: cached, built again when texture RAM under it has
    // been written (the game loads textures as it goes: the car on the
    // transmission select kept another texture's texels without this).
    const Source *source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint32_t key = uint32_t(poly.texheader[0] & 0x23ff) | uint32_t(poly.texheader[2] & 0x1fff) << 16;
        auto found = sources_.find(key);
        if (found != sources_.end()) {
            if (!stale(found->second, mem)) return &found->second;
            // (Within the build budget; otherwise the old texels this frame.
            // The old one's room in the cache is left until it is emptied.)
            if (builds_ >= kBuildsPerFrame) return &found->second;
            sources_.erase(found);
            ++rebuilds;
        }
        if (builds_ >= kBuildsPerFrame) return nullptr;
        Source s{};
        s.source_w = 32u << (poly.texheader[0] & 7);
        s.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
        const bool mirror_u = (poly.texheader[0] >> 8) & 1, mirror_v = (poly.texheader[0] >> 9) & 1;
        const uint32_t step_x = std::max(1u, s.source_w / kTextureLimit), step_y = std::max(1u, s.source_h / kTextureLimit);
        const uint32_t w = s.source_w / step_x, h = s.source_h / step_y; // one period
        s.period_w = w;
        s.period_h = h;
        s.w = mirror_u ? w * 2 : w;
        s.h = mirror_v ? h * 2 : h;
        s.u_scale = float(w) / float(s.w) / (8.0f * float(s.source_w));
        s.v_scale = float(h) / float(s.h) / (8.0f * float(s.source_h));
        const size_t bytes = size_t(s.w) * s.h / 2;
        s.texels = allocate(bytes);
        if (!s.texels) return nullptr;
        ++builds_;
        const uint32_t bx = 32u * (poly.texheader[2] & 0x3f);
        const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
        const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;
        std::memset(s.texels, 0, bytes);
        const uint32_t row_bytes = s.w / 2;
        // The chunks read: read_texel_quad's address (dword (off >> 1) of the
        // sheet, off from the texel's x and y with its x >= 1024 fold).
        const uint32_t sheet_chunk = (poly.texheader[2] & 0x1000) ? (0x200000 >> 10) : 0;
        s.chunk_lo = ~0u;
        s.chunk_hi = 0;
        s.built = s.checked = uint32_t(mem.tex_generation);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; x += 2) { // (a dword holds two texels across: every other x is enough)
                uint32_t tx = bx + x * step_x;
                const uint32_t fold = tx >= 1024 ? 512 : 0;
                if (fold) tx -= 1024;
                const uint32_t off = ((((by + y * step_y) >> 1) ^ fold) << 9) + (tx >> 1);
                const uint32_t chunk = sheet_chunk + ((((off >> 1) & 0x7ffff) * 4) >> 10);
                s.chunk_lo = std::min(s.chunk_lo, chunk);
                s.chunk_hi = std::max(s.chunk_hi, chunk);
            }
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const rt::TexelQuad q = rt::read_texel_quad(bx, by, x * step_x, x * step_x, y * step_y, y * step_y, sheet);
                const uint8_t t = uint8_t((q.t00 >> 4) & 0x0f);
                auto put = [&](uint32_t px, uint32_t py) {
                    s.texels[swizzled(px, py, row_bytes)] |= uint8_t(t << ((px & 1) * 4));
                };
                put(x, y);
                if (mirror_u) put(s.w - 1 - x, y);
                if (mirror_v) put(x, s.h - 1 - y);
                if (mirror_u && mirror_v) put(s.w - 1 - x, s.h - 1 - y);
            }
        sceKernelDcacheWritebackRange(s.texels, unsigned(bytes));
        return &sources_.emplace(key, s).first->second;
    }

    // A convex polygon as a triangle fan, in the GE's clip space: x and y the
    // screen position (-1..1) times w, w the depth.
    // The screen position is ((cx + x / pz) * kScale + kOffsetX, (cy - y /
    // pz) * kScale), and the clip coordinates are that position (as -1..1)
    // times pz: so x_clip = ax * pz + kBx * x and y_clip = ay * pz + kBy * y
    // (the screen's y runs down, clip y up: the two minuses cancel), with ax
    // and ay the polygon's own. No divide per vertex.
    bool polygon(const rt::GeoPoly &poly, const rt::Video &video, uint32_t colour, const Source *material) {
        const int n = poly.num_vertices;
        Vertex *v = vertices_ + vertices_used_;
        const float cx = float(video.crtc_x() + poly.center[0]), cy = float((384 - poly.center[1]) + video.crtc_y());
        constexpr float kBx = kScale * (2.0f / kScreenW), kBy = kScale * (2.0f / kScreenH);
        const float ax = (kOffsetX + cx * kScale - kScreenW * 0.5f) * (2.0f / kScreenW);
        const float ay = (kScreenH * 0.5f - cy * kScale) * (2.0f / kScreenH);
        const float us = material ? material->u_scale : 0.0f, vs = material ? material->v_scale : 0.0f;
        for (int i = 0; i < n; i++) {
            const rt::GeoVertex &in = poly.v[i];
            const float pz = in.p[0];
            // (a - a is 0 for a finite a, NaN for an infinite or NaN one.)
            if (!(pz > 0.0f) || pz - pz != 0.0f) return false; // (vertices_used_ not moved: the next polygon writes over these)
            Vertex &out = v[i];
            out.x = ax * pz + kBx * in.x;
            out.y = ay * pz + kBy * in.y;
            out.z = pz;
            out.u = in.p[1] * us;
            out.v = in.p[2] * vs;
            out.abgr = colour;
            if ((out.x - out.x) + (out.y - out.y) != 0.0f) return false;
        }
        // (The matrices were loaded once, before the polygons: sceGumUpdateMatrix there.)
        sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n,
                       nullptr, v);
        vertices_used_ += size_t(n);
        return true;
    }

    std::vector<Region> regions_;
    size_t region_ = 0, region_used_ = 0;
    bool flush_ = false;
    uint32_t *list_ = nullptr;
    uint32_t *palettes_ = nullptr;
    // The polygons' vertices, written through the CPU's cache (the list
    // sceGuStart gives is uncached: a store each went to memory) and written
    // back once, before the list is sent.
    Vertex *vertices_ = nullptr;
    size_t vertices_used_ = 0;
    PspGeContext ge_context_ = {}; // (the GE's state, saved and restored around the sent list)
    unsigned palettes_used_ = 0, builds_ = 0;
    uint64_t back_flushed_ = ~0ull, front_flushed_ = ~0ull; // the layers' generations written back
    std::vector<Entry> order_, order_tmp_;
    std::unordered_map<uint32_t, Source> sources_;
    std::unordered_map<uint32_t, PaletteSlot> palette_index_;
    uint8_t gamma_[256] = {};
};

} // namespace psp
