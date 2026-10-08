// gif.h - 最小・高速な GIF パーサ / LZW デコーダ (CRT 非依存)
// データはメモリマップされた読み取り専用バッファを想定。フレームは必要になった時点で逐次パースする。
#pragma once

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

struct GifFrame {
    size_t data;      // LZW データ最初のサブブロック位置
    size_t lct;       // ローカルカラーテーブル位置 (lctSize==0 なら未使用)
    u16 x, y, w, h;
    u16 lctSize;
    u32 delay;        // ms
    u16 rawDelay;     // ファイルに書かれた値 (1/100 秒)
    u8 minCode, interlace, disposal, hasTrans, trans;
};

struct Gif {
    const u8* d;
    size_t n;
    int W, H;
    size_t gct;
    int gctSize;
    GifFrame* frames;
    int count, cap;
    size_t parsePos;
    bool parseDone;
};

static inline u32 gifRd16(const u8* p) { return p[0] | (p[1] << 8); }

// サブブロック列を読み飛ばす。途中で途切れていたら n+1 を返す
static size_t gifSkipSub(const Gif& g, size_t p) {
    while (p < g.n) {
        u32 len = g.d[p++];
        if (!len) return p;
        p += len;
    }
    return g.n + 1;
}

static bool gifPush(Gif& g, const GifFrame& f) {
    if (g.count == g.cap) {
        int nc = g.cap ? g.cap * 2 : 64;
        HANDLE heap = GetProcessHeap();
        void* p = g.frames ? HeapReAlloc(heap, 0, g.frames, sizeof(GifFrame) * nc)
                           : HeapAlloc(heap, 0, sizeof(GifFrame) * nc);
        if (!p) return false;
        g.frames = (GifFrame*)p;
        g.cap = nc;
    }
    g.frames[g.count++] = f;
    return true;
}

// 次のフレームを 1 つだけパースする。終端に達したら false
static bool gifParseNext(Gif& g) {
    if (g.parseDone) return false;
    const u8* d = g.d;
    size_t n = g.n, p = g.parsePos;
    u8 disp = 0, hasT = 0, t = 0;
    u32 delay = 0;
    while (p < n) {
        u8 b = d[p++];
        if (b == 0x21) {                         // 拡張ブロック
            if (p >= n) break;
            u8 label = d[p++];
            if (label == 0xF9 && p + 5 <= n && d[p] >= 4) {   // Graphic Control Extension
                u8 fl = d[p + 1];
                disp = (fl >> 2) & 7;
                hasT = fl & 1;
                delay = gifRd16(d + p + 2);
                t = d[p + 4];
            }
            p = gifSkipSub(g, p);
        } else if (b == 0x2C) {                  // イメージディスクリプタ
            if (p + 9 > n) break;
            GifFrame f;
            f.x = (u16)gifRd16(d + p);
            f.y = (u16)gifRd16(d + p + 2);
            f.w = (u16)gifRd16(d + p + 4);
            f.h = (u16)gifRd16(d + p + 6);
            u8 fl = d[p + 8];
            p += 9;
            f.interlace = (fl >> 6) & 1;
            f.lct = 0;
            f.lctSize = 0;
            if (fl & 0x80) {
                f.lctSize = (u16)(2 << (fl & 7));
                f.lct = p;
                p += 3 * (size_t)f.lctSize;
            }
            if (p >= n) break;
            f.minCode = d[p++];
            f.data = p;
            f.disposal = disp;
            f.hasTrans = hasT;
            f.trans = t;
            f.rawDelay = (u16)delay;
            f.delay = delay ? delay * 10 : 100;        // 0 (指定なし) は 100ms。1/100 秒指定はそのまま 10ms
            if (!gifPush(g, f)) break;
            p = gifSkipSub(g, p);
            g.parsePos = p;
            if (p >= n) g.parseDone = true;
            return true;
        } else {
            break;                               // 0x3B (Trailer) または不正データ
        }
    }
    g.parseDone = true;
    g.parsePos = p;
    return false;
}

// ヘッダと最初のフレーム情報だけを読む (高速起動のため残りは遅延パース)
static bool gifOpen(Gif& g, const u8* d, size_t n) {
    g.d = d;
    g.n = n;
    if (n < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F') return false;
    g.W = (int)gifRd16(d + 6);
    g.H = (int)gifRd16(d + 8);
    u8 fl = d[10];
    size_t p = 13;
    if (fl & 0x80) {
        g.gctSize = 2 << (fl & 7);
        g.gct = p;
        p += 3 * (size_t)g.gctSize;
        if (p > n) return false;
    }
    g.parsePos = p;
    if (!gifParseNext(g)) return false;
    const GifFrame& f = g.frames[0];
    if (g.W == 0 || g.H == 0) {                 // 論理画面サイズが 0 の壊れた GIF 対策
        g.W = f.x + f.w;
        g.H = f.y + f.h;
    }
    return g.W > 0 && g.H > 0;
}

// フレームのパレットを BGRA (A=0xFF) で作る
static void gifPalette(const Gif& g, const GifFrame& f, u32* pal) {
    const u8* c = 0;
    int sz = 0;
    if (f.lctSize) {
        c = g.d + f.lct;
        sz = f.lctSize;
    } else if (g.gctSize) {
        c = g.d + g.gct;
        sz = g.gctSize;
    }
    for (int i = 0; i < 256; i++) {
        if (i < sz) {
            const u8* e = c + 3 * i;
            pal[i] = 0xFF000000u | (e[0] << 16) | (e[1] << 8) | e[2];
        } else {
            pal[i] = 0xFF000000u;
        }
    }
}

// インターレース時、ストリーム上 r 行目が実際に何行目か
static inline u32 gifInterlaceRow(u32 r, u32 h) {
    u32 n = (h + 7) / 8;
    if (r < n) return r * 8;
    r -= n;
    n = (h + 3) / 8;
    if (r < n) return 4 + r * 8;
    r -= n;
    n = (h + 1) / 4;
    if (r < n) return 2 + r * 4;
    r -= n;
    return 1 + r * 2;
}

// LZW 展開。辞書を「出力済みバッファ内の位置+長さ」で持ち、文字列をそのままコピーする高速方式。
// 戻り値は書き込んだピクセル数 (データ破損・途切れ時は途中まで)
// 辞書テーブルは呼び出し側が持つ (複数スレッドで同時にデコードできるように)
struct GifLzwTables {
    u32 start[4096];
    u16 len[4096];
};

static u32 gifLzw(const Gif& g, const GifFrame& f, u8* out, u32 cap, GifLzwTables* tab) {
    u32 minCode = f.minCode;
    if (minCode < 1 || minCode > 11) return 0;
    const u8* d = g.d;
    size_t n = g.n, p = f.data;
    u32 clear = 1u << minCode, eoi = clear + 1, next = clear + 2;
    u32 cs = minCode + 1, mask = (1u << cs) - 1;
    u64 acc = 0;
    u32 bits = 0, blk = 0, outPos = 0;
    u32 prevPos = 0, prevLen = 0;
    bool havePrev = false;
    u32* start = tab->start;
    u16* len = tab->len;

    for (;;) {
        while (bits < cs) {
            if (!blk) {
                if (p >= n) return outPos;
                blk = d[p++];
                if (!blk) return outPos;
            }
            if (p >= n) return outPos;
            acc |= (u64)d[p++] << bits;
            bits += 8;
            blk--;
        }
        u32 c = (u32)acc & mask;
        acc >>= cs;
        bits -= cs;

        if (c == clear) {
            next = clear + 2;
            cs = minCode + 1;
            mask = (1u << cs) - 1;
            havePrev = false;
            continue;
        }
        if (c == eoi) break;

        u32 pos = outPos, l;
        if (c < clear) {
            out[outPos++] = (u8)c;
            l = 1;
        } else if (!havePrev) {
            break;                                // 不正コード
        } else {
            u32 s;
            if (c < next) {
                s = start[c];
                l = len[c];
            } else if (c == next) {               // KwKwK: 直前の文字列 + その先頭文字
                s = prevPos;
                l = prevLen + 1;
            } else {
                break;                            // 不正コード
            }
            u32 room = cap - outPos;
            u32 m = l < room ? l : room;
            u8* o = out + outPos;
            const u8* src = out + s;
            for (u32 i = 0; i < m; i++) o[i] = src[i];   // 前方コピー (KwKwK の重なりも正しく処理)
            outPos += m;
        }
        if (havePrev && next < 4096) {
            start[next] = prevPos;                // 新エントリ = 直前文字列 + 今回の先頭文字 (出力上で連続)
            len[next] = (u16)(prevLen + 1);
            next++;
            if (next > mask && cs < 12) {
                cs++;
                mask = (1u << cs) - 1;
            }
        }
        havePrev = true;
        prevPos = pos;
        prevLen = l;
        if (outPos >= cap) break;
    }
    return outPos;
}
