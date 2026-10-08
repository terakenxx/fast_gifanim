// Verifies gif.h against WIC: per-frame raw indices must match. Also times first-frame decode.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <vector>
#include "../gif.h"
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static double now() { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return t.QuadPart * 1000.0 / f.QuadPart; }

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(0, COINIT_MULTITHREADED);
    IWICImagingFactory* fac;
    CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    int fails = 0;
    for (int a = 1; a < argc; a++) {
        HANDLE fh = CreateFileW(argv[a], GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        DWORD sz = GetFileSize(fh, 0), rd;
        std::vector<u8> buf(sz);
        ReadFile(fh, buf.data(), sz, &rd, 0);
        CloseHandle(fh);
        Gif g = {};
        double t0 = now();
        if (!gifOpen(g, buf.data(), sz)) { wprintf(L"FAIL open %s\n", argv[a]); fails++; continue; }
        std::vector<u8> idx((size_t)g.frames[0].w * g.frames[0].h);
        gifLzw(g, g.frames[0], idx.data(), (u32)idx.size());
        double t1 = now();
        while (gifParseNext(g)) {}
        IWICBitmapDecoder* dec = 0;
        double w0 = now();
        fac->CreateDecoderFromFilename(argv[a], 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
        IWICBitmapFrameDecode* fr0; dec->GetFrame(0, &fr0);
        UINT ww, hh; fr0->GetSize(&ww, &hh);
        std::vector<u8> tmp((size_t)ww * hh * 4);
        IWICFormatConverter* cv; fac->CreateFormatConverter(&cv);
        cv->Initialize(fr0, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom);
        cv->CopyPixels(0, ww * 4, (UINT)tmp.size(), tmp.data());
        double w1 = now();
        cv->Release(); fr0->Release();
        UINT nf = 0; dec->GetFrameCount(&nf);
        int bad = 0;
        if ((int)nf != g.count) { wprintf(L"  frame count mismatch: ours %d wic %u\n", g.count, nf); bad++; }
        for (UINT i = 0; i < nf && (int)i < g.count; i++) {
            const GifFrame& f = g.frames[i];
            IWICBitmapFrameDecode* fr; dec->GetFrame(i, &fr);
            UINT w, h; fr->GetSize(&w, &h);
            if (w != f.w || h != f.h) { wprintf(L"  frame %u size mismatch\n", i); bad++; fr->Release(); continue; }
            std::vector<u8> wic((size_t)w * h), raw((size_t)w * h), ours((size_t)w * h);
            fr->CopyPixels(0, w, (UINT)wic.size(), wic.data());
            u32 got = gifLzw(g, f, raw.data(), (u32)raw.size());
            for (u32 r = 0; r < h; r++) {
                u32 dy = f.interlace ? gifInterlaceRow(r, h) : r;
                memcpy(&ours[(size_t)dy * w], &raw[(size_t)r * w], w);
            }
            size_t diff = 0;
            for (size_t k = 0; k < wic.size(); k++) diff += wic[k] != ours[k];
            if (diff) { wprintf(L"  frame %u: %zu px differ (decoded %u/%zu)\n", i, diff, got, raw.size()); bad++; }
            fr->Release();
        }
        dec->Release();
        wprintf(L"%s %s  %dx%d frames=%d  first-frame: ours %.2fms, WIC %.2fms\n", bad ? L"NG" : L"OK", argv[a], g.W, g.H, g.count, t1 - t0, w1 - w0);
        fails += bad != 0;
    }
    return fails;
}
