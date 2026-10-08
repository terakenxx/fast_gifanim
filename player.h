// player.h - フレームの合成 (disposal 処理)、背景との合成、縮小
// UI スレッドと先読みスレッドがそれぞれ自分の Decoder / Scaler を持って同時に動けるようにしている。
#pragma once
#include "gif.h"
#include <smmintrin.h>

#define MAX_PIXELS (1u << 26)          // 64M ピクセル (8192x8192) まで

static void* plAlloc(size_t n) { return VirtualAlloc(0, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static void plFree(void* p) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

// ---------------------------------------------------------------- デコーダ (キャンバスの状態)

struct Decoder {
    u32* canvas;                        // 合成済みキャンバス (A=0 は透明)
    u32* save;                          // disposal=3 用の退避領域
    u8* idx;
    size_t idxCap;
    int cur;                            // キャンバスに入っているフレーム (-1 = 空)
    bool saveValid;
    GifLzwTables* lzw;
};

static void decFree(Decoder& d) {
    plFree(d.canvas); plFree(d.save); plFree(d.idx); plFree(d.lzw);
    d.canvas = d.save = 0;
    d.idx = 0;
    d.lzw = 0;
    d.idxCap = 0;
    d.cur = -1;
    d.saveValid = false;
}

// VirtualAlloc はゼロ初期化済み & 触ったページだけ実メモリになる
static bool decInit(Decoder& d, const Gif& g) {
    size_t px = (size_t)g.W * g.H;
    d.canvas = (u32*)plAlloc(px * 4);
    d.save = (u32*)plAlloc(px * 4);
    d.lzw = (GifLzwTables*)plAlloc(sizeof(GifLzwTables));
    d.idx = 0;
    d.idxCap = 0;
    d.cur = -1;
    d.saveValid = false;
    return d.canvas && d.save && d.lzw;
}

static void decRect(Decoder& d, const Gif& g, const GifFrame& f, int op) {   // 0:透明で消去 1:退避 2:復元
    int x0 = f.x, y0 = f.y, x1 = f.x + f.w, y1 = f.y + f.h;
    if (x1 > g.W) x1 = g.W;
    if (y1 > g.H) y1 = g.H;
    if (x0 >= x1 || y0 >= y1) return;
    size_t bytes = (size_t)(x1 - x0) * 4;
    for (int y = y0; y < y1; y++) {
        size_t o = (size_t)y * g.W + x0;
        if (op == 0) memset(d.canvas + o, 0, bytes);
        else if (op == 1) memcpy(d.save + o, d.canvas + o, bytes);
        else memcpy(d.canvas + o, d.save + o, bytes);
    }
}

static void decDraw(Decoder& d, const Gif& g, const GifFrame& f) {
    if (!f.w || !f.h || f.x >= g.W || f.y >= g.H) return;
    size_t np = (size_t)f.w * f.h;
    if (np > MAX_PIXELS) return;
    if (np > d.idxCap) {
        plFree(d.idx);
        d.idx = (u8*)plAlloc(np);
        d.idxCap = d.idx ? np : 0;
        if (!d.idx) return;
    }
    u32 got = gifLzw(g, f, d.idx, (u32)np, d.lzw);
    u32 pal[256];
    gifPalette(g, f, pal);
    if (f.hasTrans) pal[f.trans] = 0;
    u32 cols = f.w;
    if (f.x + cols > (u32)g.W) cols = g.W - f.x;
    u32 full = got / f.w, rem = got % f.w;
    for (u32 r = 0; r < f.h; r++) {
        u32 cnt = r < full ? cols : (r == full ? (rem < cols ? rem : cols) : 0);
        if (!cnt) break;                                  // データ途切れ: 以降は前フレームのまま
        u32 dy = f.y + (f.interlace ? gifInterlaceRow(r, f.h) : r);
        if (dy >= (u32)g.H) continue;
        const u8* s = d.idx + (size_t)r * f.w;
        u32* dp = d.canvas + (size_t)dy * g.W + f.x;
        if (f.hasTrans) {
            for (u32 i = 0; i < cnt; i++) {
                u32 c = pal[s[i]];
                if (c) dp[i] = c;
            }
        } else {
            for (u32 i = 0; i < cnt; i++) dp[i] = pal[s[i]];
        }
    }
}

// キャンバスを次のフレームに進める (末尾なら先頭へループ)。必要に応じて次のフレームをパースする
static int decNext(Decoder& d, Gif& g) {
    int k = d.cur + 1;
    while (k >= g.count)
        if (!gifParseNext(g)) { k = 0; break; }
    if (k == 0) {
        memset(d.canvas, 0, (size_t)g.W * g.H * 4);
    } else {
        const GifFrame& p = g.frames[d.cur];
        if (p.disposal == 2) decRect(d, g, p, 0);
        else if (p.disposal == 3 && d.saveValid) decRect(d, g, p, 2);
    }
    const GifFrame& f = g.frames[k];
    d.saveValid = f.disposal == 3;
    if (d.saveValid) decRect(d, g, f, 1);
    decDraw(d, g, f);
    d.cur = k;
    return k;
}

// ---------------------------------------------------------------- 背景との合成

// 透明部分を背景 (bg: 0 市松 / 1 暗灰 / 2 白 / 3 黒) で埋める
static void composeBg(const u32* canvas, u32* dst, int W, int H, int bgMode) {
    static const u32 bgs[4][2] = {
        {0xFF3C3C3C, 0xFF2C2C2C}, {0xFF1E1E1E, 0xFF1E1E1E}, {0xFFFFFFFF, 0xFFFFFFFF}, {0xFF000000, 0xFF000000}};
    const u32* bg = bgs[bgMode & 3];
    for (int y = 0; y < H; y++) {
        const u32* s = canvas + (size_t)y * W;
        u32* d = dst + (size_t)y * W;
        u32 a = bg[(y >> 3) & 1], b = bg[((y >> 3) & 1) ^ 1];
        for (int x = 0; x < W; x++) {
            u32 c = s[x];
            d[x] = c ? c : (((x >> 3) & 1) ? b : a);
        }
    }
}

// ---------------------------------------------------------------- 縮小 (面積平均)
// 縦横を分けて処理する。各出力画素が覆うソース範囲を 1/256 画素単位の重みで平均する (GDI の HALFTONE 相当)。
// SSE4.1 (Windows 11 の動作要件に含まれる) で 1 画素 4 チャンネルをまとめて計算し、
// 出力を横帯に分けてスレッドプールで並列に処理する。

struct ScaleTap {
    int i0, i1;                         // 覆うソース画素の範囲 (両端を含む)
    u32 w0, w1, total;                  // 両端の重み、重みの合計
    float rcp;                          // 1 / total
};

#define SCALE_MAX_BANDS 16

struct Scaler {
    ScaleTap* tx;
    ScaleTap* ty;
    u32* tmp;                           // 横方向だけ縮小した中間画像 (dw x sh)
    u32* acc;                           // 縦方向の積算 (帯ごとに 4 x dw)
    int sw, sh, dw, dh;
    int bands;
    PTP_WORK work;
    const u32* src;
    u32* dst;
    volatile LONG nextBand;
};

static void scaleTaps(ScaleTap* t, int src, int dst) {
    u64 step = ((u64)src << 16) / dst;
    for (int o = 0; o < dst; o++) {
        u64 x0 = o * step, x1 = o + 1 == dst ? (u64)src << 16 : (o + 1) * step;
        ScaleTap& a = t[o];
        a.i0 = (int)(x0 >> 16);
        a.i1 = (int)((x1 - 1) >> 16);
        if (a.i0 == a.i1) {
            a.w0 = a.w1 = a.total = 256;
        } else {
            a.w0 = (u32)((((u64)(a.i0 + 1) << 16) - x0) >> 8);
            a.w1 = (u32)((x1 - ((u64)a.i1 << 16)) >> 8);
            a.total = a.w0 + a.w1 + 256u * (u32)(a.i1 - a.i0 - 1);
            if (!a.total) a.total = 1;
        }
        a.rcp = 1.0f / (float)(int)a.total;
    }
}

static void scalerFree(Scaler& s) {
    if (s.work) {
        WaitForThreadpoolWorkCallbacks(s.work, FALSE);
        CloseThreadpoolWork(s.work);
    }
    plFree(s.tx); plFree(s.ty); plFree(s.tmp); plFree(s.acc);
    memset(&s, 0, sizeof s);
}

static void scaleBand(Scaler& s, int band);

static VOID CALLBACK scaleWorkCb(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK) {
    Scaler& s = *(Scaler*)ctx;
    for (;;) {
        LONG b = InterlockedIncrement(&s.nextBand) - 1;
        if (b >= s.bands) break;
        scaleBand(s, (int)b);
    }
}

static bool scalerSetup(Scaler& s, int sw, int sh, int dw, int dh) {
    if (s.sw == sw && s.sh == sh && s.dw == dw && s.dh == dh && s.tx) return true;
    PTP_WORK work = s.work;
    s.work = 0;
    scalerFree(s);
    s.work = work;
    if (!s.bands) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        s.bands = (int)si.dwNumberOfProcessors;
        if (s.bands > SCALE_MAX_BANDS) s.bands = SCALE_MAX_BANDS;
        if (s.bands < 1) s.bands = 1;
    }
    s.tx = (ScaleTap*)plAlloc(sizeof(ScaleTap) * dw);
    s.ty = (ScaleTap*)plAlloc(sizeof(ScaleTap) * dh);
    s.tmp = (u32*)plAlloc((size_t)dw * sh * 4);
    s.acc = (u32*)plAlloc((size_t)dw * 16 * SCALE_MAX_BANDS);
    if (!s.tx || !s.ty || !s.tmp || !s.acc) { scalerFree(s); return false; }
    scaleTaps(s.tx, sw, dw);
    scaleTaps(s.ty, sh, dh);
    s.sw = sw; s.sh = sh; s.dw = dw; s.dh = dh;
    if (!s.work && s.bands > 1) s.work = CreateThreadpoolWork(scaleWorkCb, &s, 0);
    return true;
}

// 4 チャンネルの積算値 (u32 x4) を 1/total 倍して BGRA 1 画素にまとめる
static inline u32 scPack(__m128i sum, float rcp) {
    __m128i v = _mm_cvtps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(sum), _mm_set1_ps(rcp)));
    v = _mm_packus_epi32(v, v);
    v = _mm_packus_epi16(v, v);
    return (u32)_mm_cvtsi128_si32(v) | 0xFF000000u;
}

static inline __m128i scPx(u32 p) { return _mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)p)); }

static void scaleBand(Scaler& s, int band) {
    int o0 = (int)((LONGLONG)s.dh * band / s.bands), o1 = (int)((LONGLONG)s.dh * (band + 1) / s.bands);
    if (o0 >= o1) return;
    int dw = s.dw;
    // 横方向: この帯が使うソース行だけ (帯の境界の行は隣の帯と重複して同じ値を書く)
    int y0 = s.ty[o0].i0, y1 = s.ty[o1 - 1].i1;
    for (int y = y0; y <= y1; y++) {
        const u32* sp = s.src + (size_t)y * s.sw;
        u32* tp = s.tmp + (size_t)y * dw;
        for (int o = 0; o < dw; o++) {
            const ScaleTap& a = s.tx[o];
            if (a.i0 == a.i1) { tp[o] = sp[a.i0]; continue; }
            __m128i mid = _mm_setzero_si128();
            for (int i = a.i0 + 1; i < a.i1; i++) mid = _mm_add_epi32(mid, scPx(sp[i]));
            __m128i sum = _mm_add_epi32(_mm_mullo_epi32(scPx(sp[a.i0]), _mm_set1_epi32((int)a.w0)),
                                        _mm_mullo_epi32(scPx(sp[a.i1]), _mm_set1_epi32((int)a.w1)));
            sum = _mm_add_epi32(sum, _mm_slli_epi32(mid, 8));
            tp[o] = scPack(sum, a.rcp);
        }
    }
    // 縦方向
    __m128i* acc = (__m128i*)(s.acc + (size_t)band * dw * 4);
    for (int o = o0; o < o1; o++) {
        const ScaleTap& a = s.ty[o];
        u32* dp = s.dst + (size_t)o * dw;
        if (a.i0 == a.i1) { memcpy(dp, s.tmp + (size_t)a.i0 * dw, (size_t)dw * 4); continue; }
        for (int i = a.i0; i <= a.i1; i++) {
            __m128i w = _mm_set1_epi32((int)(i == a.i0 ? a.w0 : (i == a.i1 ? a.w1 : 256)));
            const u32* tp = s.tmp + (size_t)i * dw;
            if (i == a.i0) {
                for (int x = 0; x < dw; x++) _mm_store_si128(acc + x, _mm_mullo_epi32(scPx(tp[x]), w));
            } else {
                for (int x = 0; x < dw; x++)
                    _mm_store_si128(acc + x, _mm_add_epi32(_mm_load_si128(acc + x), _mm_mullo_epi32(scPx(tp[x]), w)));
            }
        }
        for (int x = 0; x < dw; x++) dp[x] = scPack(_mm_load_si128(acc + x), a.rcp);
    }
}

// src (sw x sh) を dst (dw x dh) に縮小する。dw<=sw, dh<=sh であること
static bool scaleDown(Scaler& s, const u32* src, int sw, int sh, u32* dst, int dw, int dh) {
    if (sw / dw > 4000 || sh / dh > 4000) return false;   // 極端な縮小率は積算があふれるので扱わない
    if (!scalerSetup(s, sw, sh, dw, dh)) return false;
    s.src = src;
    s.dst = dst;
    s.nextBand = 0;
    if (s.work) {
        // 呼び出し元スレッドも帯を処理し、残りをスレッドプールに任せる
        int helpers = s.bands - 1;
        for (int i = 0; i < helpers; i++) SubmitThreadpoolWork(s.work);
        scaleWorkCb(0, &s, 0);
        WaitForThreadpoolWorkCallbacks(s.work, FALSE);
    } else {
        for (int b = 0; b < s.bands; b++) scaleBand(s, b);
    }
    return true;
}
