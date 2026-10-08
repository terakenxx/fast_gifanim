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
}

#include "gif.h"

#define APP_NAME L"FastGif"
#define MAX_PIXELS (1u << 26)          // 64M ピクセル (8192x8192) まで
#define BAR_COLOR RGB(30, 30, 30)

static Gif G;
static const u8* g_view;
static u32* g_canvas;                   // 合成済みキャンバス (A=0 は透明)
static u32* g_disp;                     // 背景と合成した表示用バッファ
static u32* g_save;                     // disposal=3 用の退避領域
static u8* g_idx;
static size_t g_idxCap;
static int g_cur = -1;                  // キャンバスに入っているフレーム (先読み済み)
static int g_shown = -1;                // 画面に出ているフレーム
static bool g_saveValid, g_loaded, g_playing, g_full, g_titleDone;
static int g_bg;                        // 0:市松 1:暗灰 2:白 3:黒
static HWND g_hwnd;
static HANDLE g_timer;
static LONGLONG g_freq, g_deadline;
static BITMAPINFO g_bmi;
static HBRUSH g_barBrush;
static WINDOWPLACEMENT g_wp;
static wchar_t g_path[32768], g_tmp[32768];

static void* valloc(size_t n) { return VirtualAlloc(0, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static void vfree(void* p) { if (p) VirtualFree(p, 0, MEM_RELEASE); }
static LONGLONG qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// ---------------------------------------------------------------- ファイル

static void closeGif() {
    g_playing = false;
    if (g_timer) CancelWaitableTimer(g_timer);
    vfree(g_canvas); vfree(g_disp); vfree(g_save); vfree(g_idx);
    g_canvas = g_disp = g_save = 0;
    g_idx = 0;
    g_idxCap = 0;
    if (g_view) UnmapViewOfFile(g_view);
    g_view = 0;
    if (G.frames) HeapFree(GetProcessHeap(), 0, G.frames);
    memset(&G, 0, sizeof G);
    g_loaded = false;
    g_cur = g_shown = -1;
    g_saveValid = false;
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
    // VirtualAlloc はゼロ初期化済み & 触ったページだけ実メモリになる
    g_canvas = (u32*)valloc(px * 4);
    g_disp = (u32*)valloc(px * 4);
    g_save = (u32*)valloc(px * 4);
    if (!g_canvas || !g_disp || !g_save) return false;
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = G.W;
    g_bmi.bmiHeader.biHeight = -G.H;    // トップダウン
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    return g_loaded = true;
}

static bool openGif(const wchar_t* path) {
    closeGif();
    if (openGifInner(path)) return true;
    closeGif();
    return false;
}

// ---------------------------------------------------------------- 合成

static bool clipFrame(const GifFrame& f, int& x0, int& y0, int& x1, int& y1) {
    x0 = f.x; y0 = f.y;
    x1 = f.x + f.w; y1 = f.y + f.h;
    if (x1 > G.W) x1 = G.W;
    if (y1 > G.H) y1 = G.H;
    return x0 < x1 && y0 < y1;
}

static void rectOp(const GifFrame& f, int op) {   // 0:透明で消去 1:退避 2:復元
    int x0, y0, x1, y1;
    if (!clipFrame(f, x0, y0, x1, y1)) return;
    size_t bytes = (size_t)(x1 - x0) * 4;
    for (int y = y0; y < y1; y++) {
        size_t o = (size_t)y * G.W + x0;
        if (op == 0) memset(g_canvas + o, 0, bytes);
        else if (op == 1) memcpy(g_save + o, g_canvas + o, bytes);
        else memcpy(g_canvas + o, g_save + o, bytes);
    }
}

static void drawFrame(const GifFrame& f) {
    if (!f.w || !f.h || f.x >= G.W || f.y >= G.H) return;
    size_t np = (size_t)f.w * f.h;
    if (np > MAX_PIXELS) return;
    if (np > g_idxCap) {
        vfree(g_idx);
        g_idx = (u8*)valloc(np);
        g_idxCap = g_idx ? np : 0;
        if (!g_idx) return;
    }
    u32 got = gifLzw(G, f, g_idx, (u32)np);
    u32 pal[256];
    gifPalette(G, f, pal);
    if (f.hasTrans) pal[f.trans] = 0;
    u32 cols = f.w;
    if (f.x + cols > (u32)G.W) cols = G.W - f.x;
    u32 full = got / f.w, rem = got % f.w;
    for (u32 r = 0; r < f.h; r++) {
        u32 cnt = r < full ? cols : (r == full ? (rem < cols ? rem : cols) : 0);
        if (!cnt) break;                                  // データ途切れ: 以降は前フレームのまま
        u32 dy = f.y + (f.interlace ? gifInterlaceRow(r, f.h) : r);
        if (dy >= (u32)G.H) continue;
        const u8* s = g_idx + (size_t)r * f.w;
        u32* dp = g_canvas + (size_t)dy * G.W + f.x;
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

// キャンバスを次のフレームに進める (末尾なら先頭へループ)
static void decodeNext() {
    int k = g_cur + 1;
    while (k >= G.count)
        if (!gifParseNext(G)) { k = 0; break; }
    if (k == 0) {
        memset(g_canvas, 0, (size_t)G.W * G.H * 4);
    } else {
        const GifFrame& p = G.frames[g_cur];
        if (p.disposal == 2) rectOp(p, 0);
        else if (p.disposal == 3 && g_saveValid) rectOp(p, 2);
    }
    const GifFrame& f = G.frames[k];
    g_saveValid = f.disposal == 3;
    if (g_saveValid) rectOp(f, 1);
    drawFrame(f);
    g_cur = k;
}

static void compose() {
    static const u32 bgs[4][2] = {
        {0xFF3C3C3C, 0xFF2C2C2C}, {0xFF1E1E1E, 0xFF1E1E1E}, {0xFFFFFFFF, 0xFFFFFFFF}, {0xFF000000, 0xFF000000}};
    const u32* bg = bgs[g_bg];
    int W = G.W, H = G.H;
    for (int y = 0; y < H; y++) {
        const u32* s = g_canvas + (size_t)y * W;
        u32* d = g_disp + (size_t)y * W;
        u32 a = bg[(y >> 3) & 1], b = bg[((y >> 3) & 1) ^ 1];
        for (int x = 0; x < W; x++) {
            u32 c = s[x];
            d[x] = c ? c : (((x >> 3) & 1) ? b : a);
        }
    }
}

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

static void present() {
    compose();
    g_shown = g_cur;
    RECT r = imageRect();
    InvalidateRect(g_hwnd, &r, FALSE);
    UpdateWindow(g_hwnd);
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
        int dw = ir.right - ir.left, dh = ir.bottom - ir.top;
        if (dw == G.W && dh == G.H) {
            SetDIBitsToDevice(dc, ir.left, ir.top, G.W, G.H, 0, 0, 0, G.H, g_disp, &g_bmi, DIB_RGB_COLORS);
        } else {
            SetStretchBltMode(dc, dw < G.W ? HALFTONE : COLORONCOLOR);   // 縮小は滑らか / 拡大はドット維持
            SetBrushOrgEx(dc, 0, 0, 0);
            StretchDIBits(dc, ir.left, ir.top, dw, dh, 0, 0, G.W, G.H, g_disp, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
        }
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

static bool isAnimated() { return g_loaded && !(G.parseDone && G.count == 1); }

static void setTitle() {
    static wchar_t b[1024];
    const wchar_t* name = g_path;
    for (const wchar_t* p = g_path; *p; p++)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    if (!g_loaded) {
        if (g_path[0]) wsprintfW(b, L"%.300s - 読み込めません - " APP_NAME, name);
        else lstrcpyW(b, APP_NAME);
    } else {
        // フレーム総数は末尾までパースし終えてから表示 (遅延パースのため)
        int n = wsprintfW(b, L"%.300s - %d×%d", name, G.W, G.H);
        if (G.parseDone) n += wsprintfW(b + n, L" - %d フレーム", G.count);
        if (!g_playing && isAnimated()) {
            n += wsprintfW(b + n, L"  [一時停止 %d", g_shown + 1);
            wsprintfW(b + n, G.parseDone ? L"/%d]" : L"]", G.count);
        }
    }
    g_titleDone = G.parseDone;
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

static void tick() {
    present();
    armAfter(G.frames[g_shown].delay, false);
    decodeNext();                                   // 次フレームを先読みしておく
    if (G.parseDone != g_titleDone) setTitle();
}

static void stopPlay() {
    g_playing = false;
    CancelWaitableTimer(g_timer);
}

static void startAfterShown(bool reset) {
    g_playing = true;
    armAfter(G.frames[g_shown].delay, reset);
}

static void togglePause() {
    if (!isAnimated()) return;
    if (g_playing) stopPlay();
    else startAfterShown(true);
    setTitle();
}

static void renderTo(int t) {
    g_cur = -1;
    do decodeNext(); while (g_cur != t);
    present();
    decodeNext();
}

static void step(int dir) {
    if (!isAnimated()) return;
    stopPlay();
    if (dir > 0) {
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
    renderTo(0);
    if (g_playing) startAfterShown(true);
    else setTitle();
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
        compose();
        g_shown = g_cur;
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

// ---------------------------------------------------------------- ウィンドウプロシージャ

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(h, 0, FALSE);
        trackNormalRect();
        return 0;
    case WM_MOVE:
        trackNormalRect();
        return 0;
    case WM_TIMER:
        if (wp == 1 && g_playing && WaitForSingleObject(g_timer, 0) == WAIT_OBJECT_0) tick();
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
        case 'B':
            if (g_loaded) {
                g_bg = (g_bg + 1) & 3;
                compose();
                InvalidateRect(h, 0, FALSE);
            }
            break;
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

    // 2) メッセージループ: 高精度 Waitable Timer とメッセージを同時に待つ
    MSG m;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(1, &g_timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) {
            if (g_playing) tick();
            continue;
        }
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) ExitProcess((UINT)m.wParam);
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}
