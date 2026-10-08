// フレームごとの処理時間を計測する (デコード / 背景合成 / 縮小: 自作 vs GDI HALFTONE)
// 使い方: bench.exe file.gif [表示幅]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "../player.h"

static double now() { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return t.QuadPart * 1000.0 / f.QuadPart; }

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    int dispW = argc > 2 ? _wtoi(argv[2]) : 0;
    HANDLE fh = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    LARGE_INTEGER sz; GetFileSizeEx(fh, &sz);
    HANDLE m = CreateFileMappingW(fh, 0, PAGE_READONLY, 0, 0, 0);
    const u8* v = (const u8*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    Gif g = {};
    double t0 = now();
    if (!gifOpen(g, v, (size_t)sz.QuadPart)) { printf("open failed\n"); return 1; }
    while (gifParseNext(g)) {}
    double tParse = now() - t0;
    long long sumDelay = 0;
    for (int i = 0; i < g.count; i++) sumDelay += g.frames[i].delay;
    printf("  %lld bytes, %dx%d, %d frames, parse-all %.1fms, avg delay %.1fms\n", sz.QuadPart, g.W, g.H, g.count, tParse,
           (double)sumDelay / g.count);
    { int hist[8] = {}; for (int i = 0; i < g.count; i++) { int r = g.frames[i].rawDelay; hist[r == 0 ? 0 : r == 1 ? 1 : r == 2 ? 2 : r <= 4 ? 3 : r <= 9 ? 4 : 5]++; }
      printf("  raw delay histogram: 0cs=%d 1cs=%d 2cs=%d 3-4cs=%d 5-9cs=%d 10cs+=%d\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]); }
    size_t px = (size_t)g.W * g.H;
    std::vector<u32> disp(px);
    int dw = dispW ? dispW : g.W, dh = (int)((long long)g.H * dw / g.W);
    std::vector<u32> scaled((size_t)dw * dh);
    Decoder d = {};
    decInit(d, g);
    Scaler sc = {};
    HDC sdc = GetDC(0), mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, dw, dh);
    SelectObject(mdc, bm);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biWidth = g.W; bmi.bmiHeader.biHeight = -g.H;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    double tD = 0, tC = 0, tS = 0, tH = 0, maxDiff = 0;
    int n = g.count;
    for (int i = 0; i < n; i++) {
        double a = now();
        decNext(d, g);
        double b = now();
        composeBg(d.canvas, disp.data(), g.W, g.H, 0);
        double c = now();
        scaleDown(sc, disp.data(), g.W, g.H, scaled.data(), dw, dh);
        double e = now();
        SetStretchBltMode(mdc, HALFTONE);
        StretchDIBits(mdc, 0, 0, dw, dh, 0, 0, g.W, g.H, disp.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        GdiFlush();
        double f = now();
        tD += b - a; tC += c - b; tS += e - c; tH += f - e;
        if (i == n / 2 && GetEnvironmentVariableW(L"BENCH_DUMP", 0, 0)) {   // 両方の結果を BMP に書き出す (目視確認用)
            wchar_t dir[MAX_PATH];
            GetEnvironmentVariableW(L"BENCH_DUMP", dir, MAX_PATH);
            std::vector<u32> hal((size_t)dw * dh);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = dw; bi.bmiHeader.biHeight = -dh;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            GetDIBits(mdc, bm, 0, dh, hal.data(), &bi, DIB_RGB_COLORS);
            const u32* imgs[2] = {scaled.data(), hal.data()};
            const wchar_t* names[2] = {L"own.bmp", L"halftone.bmp"};
            for (int k = 0; k < 2; k++) {
                wchar_t path[MAX_PATH];
                wsprintfW(path, L"%s\\%s", dir, names[k]);
                BITMAPFILEHEADER fh2 = {0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + (size_t)dw * dh * 4), 0, 0,
                                        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
                HANDLE o = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
                DWORD wr;
                WriteFile(o, &fh2, sizeof fh2, &wr, 0);
                WriteFile(o, &bi.bmiHeader, sizeof bi.bmiHeader, &wr, 0);
                WriteFile(o, imgs[k], (DWORD)((size_t)dw * dh * 4), &wr, 0);
                CloseHandle(o);
            }
        }
        if (i == n / 2) {   // 自作縮小と HALFTONE の差 (画質の確認用)
            for (int y = 0; y < dh; y += 7)
                for (int x = 0; x < dw; x += 7) {
                    COLORREF h = GetPixel(mdc, x, y);
                    u32 p = scaled[(size_t)y * dw + x];
                    int diff = abs((int)GetRValue(h) - (int)((p >> 16) & 0xFF)) + abs((int)GetGValue(h) - (int)((p >> 8) & 0xFF)) +
                               abs((int)GetBValue(h) - (int)(p & 0xFF));
                    if (diff > maxDiff) maxDiff = diff;
                }
        }
    }
    printf("  per-frame avg: decode %.2f  compose %.2f  scale(own) %.2f  scale(HALFTONE) %.2f ms   [%dx%d -> %dx%d, max RGB diff vs HALFTONE %.0f]\n",
           tD / n, tC / n, tS / n, tH / n, g.W, g.H, dw, dh, maxDiff);
    return 0;
}
