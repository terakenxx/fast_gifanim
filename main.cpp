// FastGif - 1 枚目を最速で表示するアニメーション GIF ビューワ
// CRT 非依存 / Win32 API のみ (kernel32, user32, gdi32, dwmapi) / 単体 exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <intrin.h>

extern "C" {
#pragma function(memset, memcpy)
void* __cdecl memset(void* d, int c, size_t n) {
    __stosb((unsigned char*)d, (unsigned char)c, n);
    return d;
}
void* __cdecl memcpy(void* d, const void* s, size_t n) {
    __movsb((unsigned char*)d, (const unsigned char*)s, n);
    return d;
}
int _fltused = 0;                       // 縮小処理で float を使うため (CRT なしでのリンク用)
}

#include "player.h"

#define APP_NAME L"FastGif"
#define BAR_COLOR RGB(30, 30, 30)

static Gif G;
static const u8* g_view;
static Decoder g_dec;                   // UI スレッドのデコーダ (通常モード)。canvas には先読みした次フレームが入る
static u32* g_disp;                     // 背景と合成した表示用バッファ (原寸)
static u32* g_sdisp;                    // g_disp を表示サイズに縮小したもの
static int g_sdispW, g_sdispH;
static size_t g_sdispCap;
static bool g_sdispOk;
static Scaler g_sc;
static int g_shown = -1;                // 画面に出ているフレーム
static u32 g_shownDelay;                // そのフレームの表示時間 (ms)
static bool g_loaded, g_playing, g_full, g_titleDone;
static int g_bg;                        // 0:市松 1:暗灰 2:白 3:黒
static HWND g_hwnd;
static HANDLE g_timer;
static LONGLONG g_freq, g_deadline;
static HBRUSH g_barBrush;
static WINDOWPLACEMENT g_wp;
static wchar_t g_path[32768], g_tmp[32768];

static LONGLONG qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static void setBmi(BITMAPINFO& b, int w, int h) {
    memset(&b, 0, sizeof b);
    b.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    b.bmiHeader.biWidth = w;
    b.bmiHeader.biHeight = -h;          // トップダウン
    b.bmiHeader.biPlanes = 1;
    b.bmiHeader.biBitCount = 32;
    b.bmiHeader.biCompression = BI_RGB;
}

// ---------------------------------------------------------------- 先読みキャッシュ (遅延を検知したら使う)
// 通常は UI スレッドで 1 フレームずつデコードする (1 枚目を最速で出すため)。
// 再生が間に合わない状態が続いたら、別スレッドでファイルを先読みし、フレームを表示サイズに縮小した状態で
// メモリに貯めておく方式に切り替える。メモリに入りきらない分は、再生位置から遠いフレームから入れ替える。

struct Slot {
    u32* px;                            // 背景合成・縮小済みの画素 (0 = 未作成)
    int w, h;
    LONG gen;                           // 作成時の世代 (背景色を変えたら古くなる)
    u32 delay;
};

static bool g_modeB;                    // 先読みキャッシュで再生中
static CRITICAL_SECTION g_cs;           // g_slots / g_cacheBytes を保護
static Slot* g_slots;
static int g_slotCap;
static size_t g_cacheBytes, g_budget;
static HANDLE g_worker, g_wake, g_readyEvt;
static volatile LONG g_quit, g_gen, g_tw, g_th, g_want, g_shownV, g_bCount, g_bDone;
static Decoder g_wdec;                  // 先読みスレッドのデコーダ
static bool g_waitFrame, g_waitArm;     // 次のフレームの完成待ち
static u32 g_slowHist;                  // 直近フレームの「間に合わなかった」履歴 (ビット列)
static int g_ticks;

static int frameCount() { return g_modeB ? (int)g_bCount : G.count; }
static bool parseDone() { return g_modeB ? g_bDone != 0 : G.parseDone; }

static bool slotValid(const Slot& s, int tw, int th, LONG gen) { return s.px && s.w == tw && s.h == th && s.gen == gen; }

// want から数えて何フレーム先か (総数が未確定の間は、パース済みの数で近似)
static int cacheDist(int f, int want, int count) {
    int d = f - want;
    return d < 0 ? d + count : d;
}

// 次に作るべきフレーム。全部そろっていれば -1、未パースの次フレームなら G.count を返す
static int cachePick(int want, int shown, int tw, int th, LONG gen) {
    int count = G.count;
    if (want >= count) return G.parseDone ? 0 : count;   // 総数が確定したら末尾の次は先頭
    for (int d = 0; d < count; d++) {
        int f = want + d;
        if (f >= count) {
            if (!G.parseDone) return count;     // 総数が未確定: 先頭へ戻る前に続きをパースする
            f -= count;
        }
        if (f == shown && d) continue;
        if (f >= g_slotCap || !slotValid(g_slots[f], tw, th, gen)) return f;
    }
    return G.parseDone ? -1 : count;
}

// f を置くために追い出すフレーム。古い (サイズ・世代違い) ものを優先し、無ければ f より遠いもの (-1 = 無し)
static int cacheVictim(int f, int want, int shown, int tw, int th, LONG gen) {
    int count = G.count, best = -1, bestD = cacheDist(f, want, count);
    for (int e = 0; e < g_slotCap && e < count; e++) {
        const Slot& s = g_slots[e];
        if (!s.px || e == f || e == shown) continue;
        if (!slotValid(s, tw, th, gen)) return e;
        int d = cacheDist(e, want, count);
        if (d > bestD) { bestD = d; best = e; }
    }
    return best;
}

static bool cacheHasRoom(int f, size_t bytes, int want, int shown, int tw, int th, LONG gen) {
    size_t cur = f < g_slotCap && g_slots[f].px ? (size_t)g_slots[f].w * g_slots[f].h * 4 : 0;
    return g_cacheBytes - cur + bytes <= g_budget || cacheVictim(f, want, shown, tw, th, gen) >= 0;
}

static void cacheEvictLocked(int e, u32** freeList, int* nFree) {
    Slot& s = g_slots[e];
    g_cacheBytes -= (size_t)s.w * s.h * 4;
    freeList[(*nFree)++] = s.px;
    s.px = 0;
}

static void workerPublish() {
    InterlockedExchange(&g_bCount, G.count);
    if (InterlockedExchange(&g_bDone, G.parseDone) != (LONG)G.parseDone)
        SetEvent(g_readyEvt);                       // 総数が確定した: 待っている UI に要求し直させる
}

// フレーム k (先読みデコーダのキャンバスにある) を表示用に作って登録する
static void cacheProduce(int k, Scaler& sc, u32* comp) {
    int tw = g_tw, th = g_th;
    LONG gen = g_gen;
    int want = g_want, shown = g_shownV;
    size_t bytes = (size_t)tw * th * 4;
    u32* freeList[64];
    int nFree = 0;
    EnterCriticalSection(&g_cs);
    size_t cur = k < g_slotCap && g_slots[k].px ? (size_t)g_slots[k].w * g_slots[k].h * 4 : 0;
    while (g_cacheBytes - cur + bytes > g_budget && nFree < 64) {
        int e = cacheVictim(k, want, shown, tw, th, gen);
        if (e < 0) break;
        cacheEvictLocked(e, freeList, &nFree);
    }
    bool ok = g_cacheBytes - cur + bytes <= g_budget;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < nFree; i++) plFree(freeList[i]);
    if (!ok) return;

    u32* buf = (u32*)plAlloc(bytes);
    if (!buf) return;
    int bg = g_bg;
    if (tw == G.W && th == G.H) {
        composeBg(g_wdec.canvas, buf, G.W, G.H, bg);
    } else {
        composeBg(g_wdec.canvas, comp, G.W, G.H, bg);
        if (!scaleDown(sc, comp, G.W, G.H, buf, tw, th)) { plFree(buf); return; }
    }
    u32 delay = G.frames[k].delay;

    u32* old = 0;
    EnterCriticalSection(&g_cs);
    if (k >= g_slotCap) {
        int nc = g_slotCap * 2;
        while (nc <= k) nc *= 2;
        void* p = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, g_slots, sizeof(Slot) * nc);
        if (p) {
            g_slots = (Slot*)p;
            g_slotCap = nc;
        }
    }
    if (k < g_slotCap) {
        Slot& s = g_slots[k];
        if (s.px) {
            g_cacheBytes -= (size_t)s.w * s.h * 4;
            old = s.px;
        }
        s.px = buf;
        s.w = tw;
        s.h = th;
        s.gen = gen;
        s.delay = delay;
        g_cacheBytes += bytes;
        buf = 0;
    }
    LeaveCriticalSection(&g_cs);
    plFree(old);
    plFree(buf);
    SetEvent(g_readyEvt);
}

static DWORD WINAPI cacheWorker(void*) {
    Scaler sc;
    memset(&sc, 0, sizeof sc);
    u32* comp = (u32*)plAlloc((size_t)G.W * G.H * 4);
    // 残りのファイルを OS にまとめて読み込ませる (非同期。ページフォールトでの細切れ読み込みを避ける)
    size_t from = g_wdec.cur >= 0 ? G.frames[g_wdec.cur].data : 0;
    WIN32_MEMORY_RANGE_ENTRY range = {(void*)(g_view + from), G.n - from};
    if (range.NumberOfBytes > ((size_t)2 << 30)) range.NumberOfBytes = (size_t)2 << 30;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);

    if (comp) cacheProduce(g_wdec.cur, sc, comp);   // 引き継いだキャンバスにある次フレーム
    while (comp && !g_quit) {
        int want = g_want, shown = g_shownV, tw = g_tw, th = g_th;
        LONG gen = g_gen;
        size_t bytes = (size_t)tw * th * 4;
        EnterCriticalSection(&g_cs);
        int f = cachePick(want, shown, tw, th, gen);
        bool room = f >= 0 && cacheHasRoom(f, bytes, want, shown, tw, th, gen);
        LeaveCriticalSection(&g_cs);
        if (f < 0 || !room) {                       // 全部そろった / メモリが一杯: 再生が進むまで待つ
            if (!G.parseDone) {                     // 手が空いている間に総フレーム数を確定させておく (逆方向のコマ送り用)
                for (int i = 0; i < 16 && gifParseNext(G); i++) {}
                workerPublish();
                continue;
            }
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }
        int next = g_wdec.cur + 1;
        if (G.parseDone && next >= G.count) next = 0;
        if (f < next) g_wdec.cur = -1;              // 目的のフレームを通り過ぎている: 先頭から作り直す
        int k = decNext(g_wdec, G);
        workerPublish();
        if (k == f) cacheProduce(k, sc, comp);
    }
    scalerFree(sc);
    plFree(comp);
    return 0;
}

static void stopCache() {
    if (g_worker) {
        InterlockedExchange(&g_quit, 1);
        SetEvent(g_wake);
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = 0;
    }
    if (g_slots) {
        for (int i = 0; i < g_slotCap; i++) plFree(g_slots[i].px);
        HeapFree(GetProcessHeap(), 0, g_slots);
        g_slots = 0;
    }
    g_slotCap = 0;
    g_cacheBytes = 0;
    decFree(g_wdec);
    g_modeB = false;
    g_waitFrame = false;
    g_slowHist = 0;
    g_ticks = 0;
    if (g_readyEvt) ResetEvent(g_readyEvt);
    if (g_hwnd) RemovePropW(g_hwnd, L"FastGifCache");
}

static RECT imageRect();
static bool isAnimated();

static void cacheSetTarget() {
    RECT r = imageRect();
    InterlockedExchange(&g_tw, r.right - r.left);
    InterlockedExchange(&g_th, r.bottom - r.top);
    SetEvent(g_wake);
}

// 先読みキャッシュ方式に切り替える。UI のデコーダ (次フレームまで進んだ状態) をそのまま先読みスレッドに渡す
static void startCache() {
    if (g_modeB || !isAnimated()) return;
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    g_budget = (size_t)(ms.ullAvailPhys / 2);
    if (g_budget > ((size_t)3 << 30)) g_budget = (size_t)3 << 30;
    if (g_budget < ((size_t)64 << 20)) g_budget = (size_t)64 << 20;
    wchar_t v[16];                                  // テスト用: FASTGIF_CACHE_MB でメモリ上限を指定できる
    if (GetEnvironmentVariableW(L"FASTGIF_CACHE_MB", v, 16)) {
        size_t mb = 0;
        for (const wchar_t* p = v; *p >= L'0' && *p <= L'9'; p++) mb = mb * 10 + (*p - L'0');
        if (mb) g_budget = mb << 20;
    }
    g_slotCap = G.count + 64;
    g_slots = (Slot*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Slot) * g_slotCap);
    if (!g_slots) { g_slotCap = 0; return; }
    g_cacheBytes = 0;
    g_wdec = g_dec;
    memset(&g_dec, 0, sizeof g_dec);
    g_dec.cur = -1;
    g_quit = 0;
    g_want = g_wdec.cur;
    g_shownV = g_shown;
    workerPublish();
    RECT r = imageRect();
    g_tw = r.right - r.left;
    g_th = r.bottom - r.top;
    g_modeB = true;
    g_worker = CreateThread(0, 0, cacheWorker, 0, 0, 0);
    if (!g_worker) {                                // 失敗したら通常モードのまま
        g_dec = g_wdec;
        memset(&g_wdec, 0, sizeof g_wdec);
        HeapFree(GetProcessHeap(), 0, g_slots);
        g_slots = 0;
        g_slotCap = 0;
        g_modeB = false;
        return;
    }
    SetPropW(g_hwnd, L"FastGifCache", (HANDLE)1);   // 動作確認用 (外部から先読みモードかを調べられる)
}

// 再生が間に合っているかを記録し、間に合わない状態が続いたら先読みに切り替える。
// 最初の数フレームや一時的な遅れは問題にしない (1 枚目を早く出すことを優先)
static void noteSpeed(bool slow) {
    g_ticks++;
    g_slowHist = ((g_slowHist << 1) | (slow ? 1u : 0u)) & 0xFF;
    int n = 0;
    for (u32 h = g_slowHist; h; h &= h - 1) n++;
    if (g_ticks >= 8 && n >= 6) startCache();     // 直近 8 フレーム中 6 フレームが遅れた
    else if (g_ticks == 2 && GetEnvironmentVariableW(L"FASTGIF_FORCE_CACHE", 0, 0)) startCache();   // テスト用
}

// ---------------------------------------------------------------- ファイル

static void closeGif() {
    g_playing = false;
    if (g_timer) CancelWaitableTimer(g_timer);
    stopCache();
    decFree(g_dec);
    scalerFree(g_sc);
    plFree(g_disp); plFree(g_sdisp);
    g_disp = g_sdisp = 0;
    g_sdispCap = 0;
    g_sdispOk = false;
    if (g_view) UnmapViewOfFile(g_view);
    g_view = 0;
    if (G.frames) HeapFree(GetProcessHeap(), 0, G.frames);
    memset(&G, 0, sizeof G);
    g_loaded = false;
    g_shown = -1;
}

static bool openGifInner(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0) {
        HANDLE m = CreateFileMappingW(f, 0, PAGE_READONLY, 0, 0, 0);
        if (m) {
            g_view = (const u8*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(m);
        }
    }
    CloseHandle(f);
    if (!g_view || !gifOpen(G, g_view, (size_t)sz.QuadPart)) return false;
    size_t px = (size_t)G.W * G.H;
    if (px > MAX_PIXELS) return false;
    g_disp = (u32*)plAlloc(px * 4);
    if (!g_disp || !decInit(g_dec, G)) return false;
    return g_loaded = true;
}

static bool openGif(const wchar_t* path) {
    closeGif();
    if (openGifInner(path)) return true;
    closeGif();
    return false;
}

static void decodeNext() { decNext(g_dec, G); }

// ---------------------------------------------------------------- 表示

static RECT imageRect() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right, ch = rc.bottom, dw, dh;
    if (G.W <= cw && G.H <= ch) {           // 収まる画像は等倍のまま中央に置く
        dw = G.W;
        dh = G.H;
    } else if ((LONGLONG)cw * G.H <= (LONGLONG)ch * G.W) {   // はみ出す画像は縦横比を保って縮小
        dw = cw;
        dh = (int)((LONGLONG)G.H * cw / G.W);
    } else {
        dh = ch;
        dw = (int)((LONGLONG)G.W * ch / G.H);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    RECT r = {(cw - dw) / 2, (ch - dh) / 2, 0, 0};
    r.right = r.left + dw;
    r.bottom = r.top + dh;
    return r;
}

// 通常モード: g_disp を表示サイズに縮小して g_sdisp に用意する
static bool prepareScaled(int dw, int dh) {
    if (g_sdispOk && g_sdispW == dw && g_sdispH == dh) return true;
    size_t need = (size_t)dw * dh * 4;
    if (need > g_sdispCap) {
        plFree(g_sdisp);
        g_sdisp = (u32*)plAlloc(need);
        g_sdispCap = g_sdisp ? need : 0;
        if (!g_sdisp) return false;
    }
    g_sdispOk = scaleDown(g_sc, g_disp, G.W, G.H, g_sdisp, dw, dh);
    g_sdispW = dw;
    g_sdispH = dh;
    return g_sdispOk;
}

// 通常モード: キャンバス (先読みした次フレーム) を表示する
static void present() {
    composeBg(g_dec.canvas, g_disp, G.W, G.H, g_bg);
    g_shown = g_dec.cur;
    g_shownDelay = G.frames[g_shown].delay;
    RECT r = imageRect();
    g_sdispOk = false;
    if (r.right - r.left != G.W || r.bottom - r.top != G.H) prepareScaled(r.right - r.left, r.bottom - r.top);
    InvalidateRect(g_hwnd, &r, FALSE);
    UpdateWindow(g_hwnd);
}

static void blit(HDC dc, const RECT& ir, const u32* px, int w, int h, int mode) {
    BITMAPINFO bmi;
    setBmi(bmi, w, h);
    int dw = ir.right - ir.left, dh = ir.bottom - ir.top;
    if (dw == w && dh == h) {
        SetDIBitsToDevice(dc, ir.left, ir.top, w, h, 0, 0, 0, h, px, &bmi, DIB_RGB_COLORS);
    } else {
        SetStretchBltMode(dc, mode);
        SetBrushOrgEx(dc, 0, 0, 0);
        StretchDIBits(dc, ir.left, ir.top, dw, dh, 0, 0, w, h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
}

static void paintImage(HDC dc, const RECT& ir) {
    int dw = ir.right - ir.left, dh = ir.bottom - ir.top;
    if (g_modeB) {
        bool done = false;
        EnterCriticalSection(&g_cs);
        if (g_shown >= 0 && g_shown < g_slotCap && g_slots[g_shown].px) {
            const Slot& s = g_slots[g_shown];
            blit(dc, ir, s.px, s.w, s.h, COLORONCOLOR);   // サイズ違いは作り直されるまでの間だけ
            done = true;
        }
        LeaveCriticalSection(&g_cs);
        if (done) return;
    }
    if (dw == G.W && dh == G.H) blit(dc, ir, g_disp, G.W, G.H, COLORONCOLOR);
    else if (prepareScaled(dw, dh)) blit(dc, ir, g_sdisp, dw, dh, COLORONCOLOR);
    else blit(dc, ir, g_disp, G.W, G.H, HALFTONE);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    if (g_loaded) {
        RECT ir = imageRect(), t;
        SetRect(&t, 0, 0, rc.right, ir.top);                FillRect(dc, &t, g_barBrush);
        SetRect(&t, 0, ir.bottom, rc.right, rc.bottom);     FillRect(dc, &t, g_barBrush);
        SetRect(&t, 0, ir.top, ir.left, ir.bottom);         FillRect(dc, &t, g_barBrush);
        SetRect(&t, ir.right, ir.top, rc.right, ir.bottom); FillRect(dc, &t, g_barBrush);
        paintImage(dc, ir);
    } else {
        FillRect(dc, &rc, g_barBrush);
        HFONT font = CreateFontW(-MulDiv(12, GetDpiForWindow(h), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        const wchar_t* msg = g_path[0] ? L"GIF を読み込めませんでした\n\nGIF ファイルをドロップ / Ctrl+O で開く"
                                       : L"GIF ファイルをドロップ\nまたは Ctrl+O で開く\n\nF1: 操作説明";
        RECT tr = rc;
        DrawTextW(dc, msg, -1, &tr, DT_CENTER | DT_CALCRECT | DT_NOPREFIX);
        int th = tr.bottom - tr.top;
        tr = rc;
        tr.top = (rc.bottom - th) / 2;
        DrawTextW(dc, msg, -1, &tr, DT_CENTER | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(font);
    }
    EndPaint(h, &ps);
}

static bool isAnimated() { return g_loaded && !(parseDone() && frameCount() == 1); }

static void setTitle() {
    static wchar_t b[1024];
    const wchar_t* name = g_path;
    for (const wchar_t* p = g_path; *p; p++)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    bool done = parseDone();
    int count = frameCount();
    if (!g_loaded) {
        if (g_path[0]) wsprintfW(b, L"%.300s - 読み込めません - " APP_NAME, name);
        else lstrcpyW(b, APP_NAME);
    } else {
        // フレーム総数は末尾までパースし終えてから表示 (遅延パースのため)
        int n = wsprintfW(b, L"%.300s - %d×%d", name, G.W, G.H);
        if (done) n += wsprintfW(b + n, L" - %d フレーム", count);
        if (!g_playing && isAnimated()) {
            n += wsprintfW(b + n, L"  [一時停止 %d", g_shown + 1);
            wsprintfW(b + n, done ? L"/%d]" : L"]", count);
        }
    }
    g_titleDone = done;
    SetWindowTextW(g_hwnd, b);
}

// ---------------------------------------------------------------- 再生制御

static void armAfter(u32 ms, bool reset) {
    LONGLONG now = qpc(), step = (LONGLONG)ms * g_freq / 1000;
    if (reset || g_deadline + step < now) g_deadline = now;   // 大きく遅れたら追いつこうとしない
    g_deadline += step;
    LONGLONG dt = g_deadline - now;
    if (dt < 0) dt = 0;
    LARGE_INTEGER due;
    due.QuadPart = -(dt * 10000000 / g_freq);
    if (!due.QuadPart) due.QuadPart = -1;
    SetWaitableTimer(g_timer, &due, 0, 0, 0, FALSE);
}

static void countPresented() {          // 動作確認用: 表示したフレーム数 (外部から再生速度を測れる)
    static UINT_PTR n;
    SetPropW(g_hwnd, L"FastGifFrames", (HANDLE)++n);
}

// 先読みモード: 次に表示したいフレームを伝える
static void setWant(int f) {
    InterlockedExchange(&g_want, f);
    SetEvent(g_wake);
}

// 先読みモード: g_want のフレームが出来ていれば表示する。無ければ完成を待つ
static bool showWanted(bool arm) {
    int f = g_want;
    if (g_bDone && f >= (int)g_bCount) {            // 総数が確定する前に「末尾の次」を要求していた
        f = 0;
        setWant(0);
    }
    bool ok;
    EnterCriticalSection(&g_cs);
    ok = f < g_slotCap && g_slots[f].px;
    if (ok) g_shownDelay = g_slots[f].delay;
    LeaveCriticalSection(&g_cs);
    if (!ok) {
        g_waitFrame = true;
        g_waitArm = arm;
        return false;
    }
    g_waitFrame = false;
    g_shown = f;
    InterlockedExchange(&g_shownV, f);
    int nx = f + 1;
    if (g_bDone && nx >= (int)g_bCount) nx = 0;
    setWant(nx);
    RECT r = imageRect();
    InvalidateRect(g_hwnd, &r, FALSE);
    UpdateWindow(g_hwnd);
    countPresented();
    if (arm) armAfter(g_shownDelay, false);
    if (!g_playing || parseDone() != g_titleDone) setTitle();   // 一時停止中はフレーム番号を出している
    return true;
}

static void tick() {
    if (g_modeB) {
        showWanted(true);
        return;
    }
    LONGLONG t0 = qpc();
    present();
    countPresented();
    u32 delay = g_shownDelay;
    armAfter(delay, false);
    decodeNext();                                   // 次フレームを先読みしておく
    LONGLONG ms = (qpc() - t0) * 1000 / g_freq;
    if (G.parseDone != g_titleDone) setTitle();
    noteSpeed(ms >= (LONGLONG)delay);
}

static void stopPlay() {
    g_playing = false;
    CancelWaitableTimer(g_timer);
}

static void startAfterShown(bool reset) {
    g_playing = true;
    armAfter(g_shownDelay, reset);
}

static void togglePause() {
    if (!isAnimated()) return;
    if (g_playing) stopPlay();
    else startAfterShown(true);
    setTitle();
}

static void renderTo(int t) {
    g_dec.cur = -1;
    do decodeNext(); while (g_dec.cur != t);
    present();
    decodeNext();
}

static void step(int dir) {
    if (!isAnimated()) return;
    stopPlay();
    if (g_modeB) {
        // 前の要求がまだ完成待ちなら、その要求を基準に進める (連打を取りこぼさない)
        int base = g_waitFrame ? (int)g_want : g_shown;
        int t = base + dir;
        if (t < 0) {
            if (!g_bDone) return;
            t = (int)g_bCount - 1;
        }
        if (g_bDone && t >= (int)g_bCount) t = 0;
        setWant(t);
        showWanted(false);
    } else if (dir > 0) {
        present();
        decodeNext();
    } else {
        int t = g_shown - 1;
        if (t < 0) {
            while (gifParseNext(G)) {}
            t = G.count - 1;
        }
        renderTo(t);
    }
    setTitle();
}

static void goFirst() {
    if (!isAnimated()) return;
    if (g_modeB) {
        setWant(0);
        if (showWanted(false) && g_playing) startAfterShown(true);
        else if (g_waitFrame) g_waitArm = g_playing;
        if (!g_playing) setTitle();
        return;
    }
    renderTo(0);
    if (g_playing) startAfterShown(true);
    else setTitle();
}

// 背景色を変えたら表示中の画像も作り直す
static void changeBg() {
    if (!g_loaded) return;
    g_bg = (g_bg + 1) & 3;
    if (g_modeB) {
        InterlockedIncrement(&g_gen);
        SetEvent(g_wake);
    } else if (!g_playing) {
        // 一時停止中: キャンバスには先読みした次フレームが入っているので、表示中のフレームを作り直す
        // (再生中は次のフレームから新しい背景になる)
        renderTo(g_shown);
    }
    InvalidateRect(g_hwnd, 0, FALSE);
}

// ---------------------------------------------------------------- ウィンドウ位置・サイズの保存 (ini)
// 保存先は exe と同じフォルダの fastgif.ini。書き込めない場合 (Program Files 等) は %APPDATA%\FastGif\fastgif.ini
static wchar_t g_ini[MAX_PATH + 16], g_iniApp[MAX_PATH + 32];
static bool g_startMax;
static RECT g_normRect;                 // 通常表示時のウィンドウ矩形 (スクリーン座標)

static void iniPath() {
    DWORD n = GetModuleFileNameW(0, g_ini, MAX_PATH);
    while (n > 0 && g_ini[n - 1] != L'\\') n--;
    lstrcpyW(g_ini + n, L"fastgif.ini");
    DWORD a = GetEnvironmentVariableW(L"APPDATA", g_iniApp, MAX_PATH);
    if (a && a < MAX_PATH) lstrcpyW(g_iniApp + a, L"\\FastGif\\fastgif.ini");
    else g_iniApp[0] = 0;
}

// 読み込み元: exe フォルダの ini が書き込み可能ならそれ (保存先と同じ)。
// 書き込めない ini しか無い / exe フォルダに ini が無い場合だけ %APPDATA% 側を読む
static const wchar_t* iniForRead() {
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0, 0);   // 開くだけなので内容・更新日時は変わらない
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        return g_ini;
    }
    return g_iniApp[0] ? g_iniApp : g_ini;
}

static int iniInt(const wchar_t* ini, const wchar_t* key, int def) {
    wchar_t v[24];
    if (!GetPrivateProfileStringW(L"Window", key, L"", v, 24, ini)) return def;
    int sign = 1, x = 0;
    const wchar_t* p = v;
    if (*p == L'-') { sign = -1; p++; }
    if (*p < L'0' || *p > L'9') return def;
    for (; *p >= L'0' && *p <= L'9'; p++) x = x * 10 + (*p - L'0');
    return x * sign;
}

// 前回の位置・サイズを読み、見えるモニタ上にあればその矩形を返す
static bool loadWindowRect(RECT* r) {
    const wchar_t* ini = iniForRead();
    const int NONE = -0x7FFFFFFF;
    int x = iniInt(ini, L"Left", NONE), y = iniInt(ini, L"Top", NONE);
    int w = iniInt(ini, L"Width", 0), h = iniInt(ini, L"Height", 0);
    g_startMax = iniInt(ini, L"Maximized", 0) != 0;
    if (x == NONE || y == NONE || w < 160 || h < 120) {
        // 旧形式 (サイズのみ) の ini はサイズだけ引き継ぐ
        SetRect(r, 0, 0, w, h);
        return false;
    }
    SetRect(r, x, y, x + w, y + h);
    HMONITOR mon = MonitorFromRect(r, MONITOR_DEFAULTTONULL);   // 外したモニタ上にあった場合は使わない
    if (!mon) return false;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetRect(r, x, y, x + w, y + h);
    return true;
}

// 保存位置が無い場合: 指定サイズ (無ければ既定サイズ) でマウスのあるモニタの中央に配置する
static void centerWindow(int w, int h) {
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int waw = wa.right - wa.left, wah = wa.bottom - wa.top;
    UINT dpi = GetDpiForWindow(g_hwnd);
    if (w < 160 || h < 120) {
        w = MulDiv(960, dpi, 96);
        h = MulDiv(680, dpi, 96);
    }
    if (w > waw) w = waw;
    if (h > wah) h = wah;
    SetWindowPos(g_hwnd, 0, wa.left + (waw - w) / 2, wa.top + (wah - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static bool writeIni(const wchar_t* ini) {
    RECT r = g_normRect;
    wchar_t v[16];
    wsprintfW(v, L"%d", r.left);
    if (!WritePrivateProfileStringW(L"Window", L"Left", v, ini)) return false;
    wsprintfW(v, L"%d", r.top);
    WritePrivateProfileStringW(L"Window", L"Top", v, ini);
    wsprintfW(v, L"%d", r.right - r.left);
    WritePrivateProfileStringW(L"Window", L"Width", v, ini);
    wsprintfW(v, L"%d", r.bottom - r.top);
    WritePrivateProfileStringW(L"Window", L"Height", v, ini);
    bool max = g_full ? g_wp.showCmd == SW_SHOWMAXIMIZED : IsZoomed(g_hwnd) != 0;
    return WritePrivateProfileStringW(L"Window", L"Maximized", max ? L"1" : L"0", ini) != 0;
}

static void saveWindowRect() {
    if (g_normRect.right <= g_normRect.left) return;
    if (writeIni(g_ini) || !g_iniApp[0]) return;
    // exe のフォルダに書けない → %APPDATA%\FastGif に保存
    int n = lstrlenW(g_iniApp);
    while (n > 0 && g_iniApp[n - 1] != L'\\') n--;
    g_iniApp[n - 1] = 0;
    CreateDirectoryW(g_iniApp, 0);
    g_iniApp[n - 1] = L'\\';
    writeIni(g_iniApp);
}

// 通常表示 (最大化・最小化・フルスクリーン以外) のときの矩形を記録しておく
static void trackNormalRect() {
    if (!g_full && !IsIconic(g_hwnd) && !IsZoomed(g_hwnd)) GetWindowRect(g_hwnd, &g_normRect);
}

static void toggleFull() {
    DWORD st = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    if (!g_full) {
        MONITORINFO mi = {sizeof mi};
        g_wp.length = sizeof g_wp;
        GetWindowPlacement(g_hwnd, &g_wp);
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_hwnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        RECT m = mi.rcMonitor;
        SetWindowPos(g_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full = true;
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full = false;
    }
}

// ---------------------------------------------------------------- 読み込み

// 1 枚目がキャンバスにある状態から表示・再生を開始する
static void showLoaded() {
    g_titleDone = false;
    if (g_loaded) {
        composeBg(g_dec.canvas, g_disp, G.W, G.H, g_bg);
        g_sdispOk = false;
        g_shown = g_dec.cur;
        g_shownDelay = G.frames[g_shown].delay;
        g_playing = true;                   // 単一フレームと判明したら下で止める
    }
    setTitle();
    InvalidateRect(g_hwnd, 0, FALSE);
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, g_startMax ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(g_hwnd);
    if (g_loaded) {
        startAfterShown(true);
        decodeNext();                       // 2 枚目を先読み (ここで単一フレームか判明する)
        if (!isAnimated()) stopPlay();
        if (G.parseDone != g_titleDone) setTitle();
    }
}

// ---- フォルダ内ナビゲーション
// 起動を速くするため、フォルダの走査は最初にホイール等で前後移動したときに 1 度だけ行い、結果を保持する
static wchar_t* g_names;                // ファイル名を \0 区切りで連結
static u32* g_order;                    // g_names 内のオフセット (ソート済み)
static size_t g_namesLen, g_namesCap;
static int g_nNames, g_orderCap, g_listPos;
static bool g_listValid, g_listFound;

static void freeList() {
    HANDLE heap = GetProcessHeap();
    if (g_names) HeapFree(heap, 0, g_names);
    if (g_order) HeapFree(heap, 0, g_order);
    g_names = 0;
    g_order = 0;
    g_namesLen = g_namesCap = 0;
    g_nNames = g_orderCap = 0;
    g_listValid = false;
}

static void* grow(void* p, size_t bytes) {
    HANDLE heap = GetProcessHeap();
    return p ? HeapReAlloc(heap, 0, p, bytes) : HeapAlloc(heap, 0, bytes);
}

static bool addName(const wchar_t* nm) {
    size_t len = (size_t)lstrlenW(nm) + 1;
    if (g_namesLen + len > g_namesCap) {
        size_t nc = g_namesCap ? g_namesCap * 2 : 16384;
        while (nc < g_namesLen + len) nc *= 2;
        void* p = grow(g_names, nc * sizeof(wchar_t));
        if (!p) return false;
        g_names = (wchar_t*)p;
        g_namesCap = nc;
    }
    if (g_nNames == g_orderCap) {
        int nc = g_orderCap ? g_orderCap * 2 : 1024;
        void* p = grow(g_order, nc * sizeof(u32));
        if (!p) return false;
        g_order = (u32*)p;
        g_orderCap = nc;
    }
    memcpy(g_names + g_namesLen, nm, len * sizeof(wchar_t));
    g_order[g_nNames++] = (u32)g_namesLen;
    g_namesLen += len;
    return true;
}

// エクスプローラと同様、数字部分は数値として比較する
static int cmpName(const wchar_t* a, const wchar_t* b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a, -1, b, -1, 0, 0, 0) - 2;
}

static void sortList() {                // ボトムアップのマージソート
    int n = g_nNames;
    u32* tmp = (u32*)HeapAlloc(GetProcessHeap(), 0, (size_t)n * sizeof(u32) + 4);
    if (!tmp) return;
    u32 *a = g_order, *b = tmp;
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) b[k++] = cmpName(g_names + a[j], g_names + a[i]) < 0 ? a[j++] : a[i++];
            while (i < mid) b[k++] = a[i++];
            while (j < hi) b[k++] = a[j++];
        }
        u32* t = a; a = b; b = t;
    }
    if (a != g_order) memcpy(g_order, a, (size_t)n * sizeof(u32));
    HeapFree(GetProcessHeap(), 0, tmp);
}

static int dirLen() {                   // g_path のフォルダ部分の長さ (末尾の \ を含む)
    int d = -1;
    for (int i = 0; g_path[i]; i++)
        if (g_path[i] == L'\\' || g_path[i] == L'/') d = i;
    return d + 1;
}

static bool scanFolder() {
    int d = dirLen();
    if (d <= 0 || d + 6 >= 32768) return false;
    freeList();
    lstrcpynW(g_tmp, g_path, d + 1);
    lstrcpyW(g_tmp + d, L"*.gif");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(g_tmp, FindExInfoBasic, &fd, FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int len = lstrlenW(fd.cFileName);
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && len >= 4 && !lstrcmpiW(fd.cFileName + len - 4, L".gif"))
                addName(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    sortList();
    // 現在のファイルの位置。一覧に無ければ挿入位置を覚えておく
    const wchar_t* cur = g_path + d;
    g_listFound = false;
    g_listPos = 0;
    for (int i = 0; i < g_nNames; i++) {
        const wchar_t* nm = g_names + g_order[i];
        if (!lstrcmpiW(nm, cur)) { g_listPos = i; g_listFound = true; break; }
        if (cmpName(nm, cur) < 0) g_listPos = i + 1;
    }
    g_listValid = true;
    return true;
}

static void loadFileInner(const wchar_t* p) {
    if (!GetFullPathNameW(p, 32768, g_path, 0)) lstrcpynW(g_path, p, 32768);
    if (openGif(g_path)) decodeNext();
    showLoaded();
}

// D&D・ダイアログで開いた場合はフォルダが変わり得るので一覧を破棄する
static void loadFile(const wchar_t* p) {
    freeList();
    loadFileInner(p);
}

// 前後のファイルを開く (steps: +1 で次、-1 で前。端では反対側へ回り込む)
static void openSibling(int steps) {
    if (!g_path[0] || !steps) return;
    if (!g_listValid && !scanFolder()) return;
    int n = g_nNames;
    if (!n) return;
    int i = g_listFound ? g_listPos + steps : (steps > 0 ? g_listPos + steps - 1 : g_listPos + steps);
    i %= n;
    if (i < 0) i += n;
    if (g_listFound && i == g_listPos) return;
    int d = dirLen();
    const wchar_t* nm = g_names + g_order[i];
    if (d + lstrlenW(nm) >= 32768) return;
    lstrcpynW(g_tmp, g_path, d + 1);
    lstrcpyW(g_tmp + d, nm);
    g_listPos = i;
    g_listFound = true;
    loadFileInner(g_tmp);
}

// モーダルループ中 (ウィンドウ移動・ダイアログ等) もアニメーションを止めないための補助タイマ
static void modalTimer(bool on) {
    if (on) SetTimer(g_hwnd, 1, 4, 0);
    else KillTimer(g_hwnd, 1);
}

static void openDialog() {
    typedef BOOL(WINAPI * PGOFN)(LPOPENFILENAMEW);
    HMODULE h = LoadLibraryW(L"comdlg32.dll");
    PGOFN fn = h ? (PGOFN)GetProcAddress(h, "GetOpenFileNameW") : 0;
    if (!fn) return;
    OPENFILENAMEW o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = g_hwnd;
    o.lpstrFilter = L"GIF 画像 (*.gif)\0*.gif\0すべてのファイル (*.*)\0*.*\0";
    g_tmp[0] = 0;
    o.lpstrFile = g_tmp;
    o.nMaxFile = 32768;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    modalTimer(true);
    BOOL ok = fn(&o);
    modalTimer(false);
    if (ok) loadFile(g_tmp);
}

static void showHelp() {
    modalTimer(true);
    MessageBoxW(g_hwnd,
                L"Space\t再生 / 一時停止\n"
                L"← / →\tコマ送り (一時停止)\n"
                L"Home\t最初のフレームへ\n"
                L"マウスホイール\t前 / 次の GIF (同じフォルダ)\n"
                L"PageUp / PageDown, ↑ / ↓\t前 / 次の GIF\n"
                L"F / F11 / Enter / ダブルクリック\tフルスクリーン\n"
                L"B\t透過部分の背景切替\n"
                L"Ctrl+O\tファイルを開く (ドラッグ&ドロップも可)\n"
                L"Esc\tフルスクリーン解除 / 終了",
                APP_NAME L" - 操作説明", MB_OK | MB_ICONINFORMATION);
    modalTimer(false);
}

// 先読みモードで待っていたフレームが出来た
static void onFrameReady() {
    if (g_modeB && g_waitFrame) showWanted(g_waitArm && g_playing);
}

// ---------------------------------------------------------------- ウィンドウプロシージャ

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_modeB) cacheSetTarget();
        InvalidateRect(h, 0, FALSE);
        trackNormalRect();
        return 0;
    case WM_MOVE:
        trackNormalRect();
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            if (g_playing && WaitForSingleObject(g_timer, 0) == WAIT_OBJECT_0) tick();
            if (g_readyEvt && WaitForSingleObject(g_readyEvt, 0) == WAIT_OBJECT_0) onFrameReady();
        }
        return 0;
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        modalTimer(true);
        return 0;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        modalTimer(false);
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DROPFILES: {
        typedef UINT(WINAPI * PDQ)(HANDLE, UINT, LPWSTR, UINT);
        typedef void(WINAPI * PDF)(HANDLE);
        HMODULE sh = LoadLibraryW(L"shell32.dll");
        PDQ q = sh ? (PDQ)GetProcAddress(sh, "DragQueryFileW") : 0;
        PDF fin = sh ? (PDF)GetProcAddress(sh, "DragFinish") : 0;
        bool got = q && q((HANDLE)wp, 0, g_tmp, 32768);
        if (fin) fin((HANDLE)wp);
        if (got) loadFile(g_tmp);
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case VK_SPACE: togglePause(); break;
        case VK_RIGHT: step(1); break;
        case VK_LEFT: step(-1); break;
        case VK_HOME: goFirst(); break;
        case VK_NEXT: case VK_DOWN: openSibling(1); break;
        case VK_PRIOR: case VK_UP: openSibling(-1); break;
        case 'F': case VK_F11: case VK_RETURN: toggleFull(); break;
        case 'B': changeBg(); break;
        case 'O': if (ctrl) openDialog(); break;
        case VK_F1: showHelp(); break;
        case VK_ESCAPE:
            if (g_full) toggleFull();
            else DestroyWindow(h);
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {               // 上で前のファイル、下で次のファイル (高解像度ホイールは 120 単位に積算)
        static int acc;
        acc += GET_WHEEL_DELTA_WPARAM(wp);
        int steps = 0;
        while (acc >= WHEEL_DELTA) { acc -= WHEEL_DELTA; steps--; }
        while (acc <= -WHEEL_DELTA) { acc += WHEEL_DELTA; steps++; }
        openSibling(steps);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        toggleFull();
        return 0;
    case WM_RBUTTONUP:
        showHelp();
        return 0;
    case WM_DESTROY:
        saveWindowRect();
        stopCache();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- エントリポイント

static DWORD WINAPI decodeFirstThread(void*) {
    decodeNext();
    return 0;
}

// 最初の引数をファイルパスとして取る。"..." で囲まれていればその中身、無ければ残り全体 (空白を含むパス対策)
static void parseCmdLine() {
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p) p++;
    } else {
        while (*p > L' ') p++;
    }
    while (*p == L' ' || *p == L'\t') p++;
    int n = 0;
    if (*p == L'"') {
        for (p++; *p && *p != L'"' && n < 32767; p++) g_tmp[n++] = *p;
    } else {
        for (; *p && n < 32767; p++) g_tmp[n++] = *p;
        while (n > 0 && (g_tmp[n - 1] == L' ' || g_tmp[n - 1] == L'\t')) n--;
    }
    g_tmp[n] = 0;
}

extern "C" void entry() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE hi = GetModuleHandleW(0);
    InitializeCriticalSection(&g_cs);

    // 1) ファイルを開いてヘッダ解析 → 1 枚目のデコードを別スレッドで開始し、並行してウィンドウを作る
    parseCmdLine();
    iniPath();
    HANDLE th = 0;
    if (g_tmp[0]) {
        if (!GetFullPathNameW(g_tmp, 32768, g_path, 0)) lstrcpynW(g_path, g_tmp, 32768);
        if (openGif(g_path)) {
            th = CreateThread(0, 0, decodeFirstThread, 0, 0, 0);
            if (!th) decodeNext();
        }
    }

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerW(0, FALSE, 0);
    g_wake = CreateEventW(0, FALSE, FALSE, 0);
    g_readyEvt = CreateEventW(0, FALSE, FALSE, 0);
    g_barBrush = CreateSolidBrush(BAR_COLOR);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = APP_NAME;
    RegisterClassExW(&wc);

    // 前回の位置があればそこに直接作る (別 DPI のモニタへ後から移動するとサイズが変わるため)
    RECT wr;
    bool havePos = loadWindowRect(&wr);
    int lastW = wr.right - wr.left, lastH = wr.bottom - wr.top;   // 位置が使えなくてもサイズは引き継ぐ
    if (!havePos) {
        POINT pt;
        GetCursorPos(&pt);                  // マウスのあるモニタに表示
        SetRect(&wr, pt.x, pt.y, pt.x + 200, pt.y + 200);
    }
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, APP_NAME, APP_NAME, WS_OVERLAPPEDWINDOW,
                             wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, 0, 0, hi, 0);
    BOOL t = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &t, sizeof t);   // 表示アニメーションを省略
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &t, sizeof t);
    if (!havePos) centerWindow(lastW, lastH);
    trackNormalRect();

    if (th) {
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    }
    showLoaded();

    // 2) メッセージループ: 高精度 Waitable Timer・フレーム完成通知・メッセージを同時に待つ
    HANDLE waits[2] = {g_timer, g_readyEvt};
    MSG m;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(2, waits, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) {
            if (g_playing) tick();
            continue;
        }
        if (r == WAIT_OBJECT_0 + 1) {
            onFrameReady();
            continue;
        }
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) ExitProcess((UINT)m.wParam);
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}
