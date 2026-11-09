// video_thumb.c - see include/video_thumb.h.
//
// IMFSourceReader with video processing on: Media Foundation picks the
// decoder (H.264, HEVC when its extension is installed, MJPEG, ...) and
// converts to RGB32 for us.  Every MF entry point is resolved with
// GetProcAddress and the few GUIDs are defined here, so nothing links
// against mfplat / mfreadwrite and the exe runs on systems without them.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601      // InitOnceExecuteOnce (Windows 7+)
#endif
#include "video_thumb.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdlib.h>
#include <string.h>

bool video_is_video_name(const char* name) {
    if (!name) return false;
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !_stricmp(dot, ".mov") || !_stricmp(dot, ".mp4") || !_stricmp(dot, ".m4v");
}

// GUIDs (values from the Windows SDK headers)
static const GUID kReaderVideoProcessing =
    {0xfb394f3d, 0xccf1, 0x42ee, {0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d}};
static const GUID kMajorType =
    {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID kSubtype =
    {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID kFrameSize =
    {0x1652c33d, 0xd6b2, 0x4012, {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
static const GUID kDefaultStride =
    {0x644b4e48, 0x1e02, 0x4516, {0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6}};
static const GUID kVideo =
    {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kRGB32 =
    {0x00000016, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kDuration =
    {0x6c990d33, 0xbb8e, 0x477a, {0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a}};
static const GUID kNull = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

#define READER_FIRST_VIDEO 0xFFFFFFFCu
#define READER_MEDIASOURCE 0xFFFFFFFFu
#define MF_VER_WIN7        0x00020070u

typedef HRESULT (WINAPI* PFN_Startup)(ULONG, DWORD);
typedef HRESULT (WINAPI* PFN_CreateAttributes)(IMFAttributes**, UINT32);
typedef HRESULT (WINAPI* PFN_CreateMediaType)(IMFMediaType**);
typedef HRESULT (WINAPI* PFN_CreateReader)(LPCWSTR, IMFAttributes*, IMFSourceReader**);

static PFN_CreateAttributes p_attr;
static PFN_CreateMediaType  p_type;
static PFN_CreateReader     p_reader;
static INIT_ONCE g_mf_once = INIT_ONCE_STATIC_INIT;
static bool g_mf_ok = false;

static BOOL CALLBACK mf_init(PINIT_ONCE o, PVOID a, PVOID* c) {
    (void)o; (void)a; (void)c;
    HMODULE plat = LoadLibraryA("mfplat.dll");
    HMODULE rw = LoadLibraryA("mfreadwrite.dll");
    if (!plat || !rw) return TRUE;
    PFN_Startup st = (PFN_Startup)(void*)GetProcAddress(plat, "MFStartup");
    p_attr = (PFN_CreateAttributes)(void*)GetProcAddress(plat, "MFCreateAttributes");
    p_type = (PFN_CreateMediaType)(void*)GetProcAddress(plat, "MFCreateMediaType");
    p_reader = (PFN_CreateReader)(void*)GetProcAddress(rw, "MFCreateSourceReaderFromURL");
    if (!st || !p_attr || !p_type || !p_reader) return TRUE;
    g_mf_ok = SUCCEEDED(st(MF_VER_WIN7, 1 /* MFSTARTUP_LITE: no sockets */));
    return TRUE;   // MF stays started for the life of the process
}

bool video_frame_preview(const char* path, int max_w, int max_h,
                         unsigned char** out_bgra, int* out_w, int* out_h) {
    if (!path || !out_bgra || !out_w || !out_h) return false;
    *out_bgra = NULL;
    InitOnceExecuteOnce(&g_mf_once, mf_init, NULL, NULL);
    if (!g_mf_ok) return false;
    wchar_t wp[1024];
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wp, 1024) <= 0) return false;

    bool ok = false;
    IMFAttributes* at = NULL;
    IMFSourceReader* rd = NULL;
    IMFMediaType* want = NULL;
    IMFMediaType* got = NULL;
    IMFSample* smp = NULL;
    IMFMediaBuffer* buf = NULL;
    if (FAILED(p_attr(&at, 1))) goto done;
    at->lpVtbl->SetUINT32(at, &kReaderVideoProcessing, TRUE);
    if (FAILED(p_reader(wp, at, &rd))) goto done;
    if (FAILED(p_type(&want))) goto done;
    want->lpVtbl->SetGUID(want, &kMajorType, &kVideo);
    want->lpVtbl->SetGUID(want, &kSubtype, &kRGB32);
    if (FAILED(rd->lpVtbl->SetCurrentMediaType(rd, READER_FIRST_VIDEO, NULL, want)))
        goto done;
    rd->lpVtbl->SetStreamSelection(rd, 0xFFFFFFFEu /* all */, FALSE);
    rd->lpVtbl->SetStreamSelection(rd, READER_FIRST_VIDEO, TRUE);
    if (FAILED(rd->lpVtbl->GetCurrentMediaType(rd, READER_FIRST_VIDEO, &got))) goto done;
    UINT64 fs = 0;
    if (FAILED(got->lpVtbl->GetUINT64(got, &kFrameSize, &fs))) goto done;
    int fw = (int)(fs >> 32), fh = (int)(fs & 0xffffffffu);
    if (fw < 1 || fh < 1 || fw > 16384 || fh > 16384) goto done;
    UINT32 sv = 0;
    LONG stride = fw * 4;
    if (SUCCEEDED(got->lpVtbl->GetUINT32(got, &kDefaultStride, &sv))) stride = (LONG)(INT32)sv;

    // 10% in, like Explorer (a fade-in must not give a black tile)
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(rd->lpVtbl->GetPresentationAttribute(rd, READER_MEDIASOURCE, &kDuration, &pv)) &&
        pv.vt == VT_UI8 && pv.uhVal.QuadPart > 0) {
        LONGLONG t = (LONGLONG)(pv.uhVal.QuadPart / 10);
        PropVariantClear(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = t;
        rd->lpVtbl->SetCurrentPosition(rd, &kNull, &pv);
    }
    PropVariantClear(&pv);

    for (int tries = 0; tries < 30 && !smp; tries++) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        if (FAILED(rd->lpVtbl->ReadSample(rd, READER_FIRST_VIDEO, 0, &idx, &flags, &ts, &smp)))
            goto done;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!smp || FAILED(smp->lpVtbl->ConvertToContiguousBuffer(smp, &buf))) goto done;
    BYTE* p = NULL;
    DWORD maxlen = 0, cur = 0;
    if (FAILED(buf->lpVtbl->Lock(buf, &p, &maxlen, &cur)) || !p) goto done;
    LONG ast = stride < 0 ? -stride : stride;
    if (ast < fw * 4 || (unsigned long long)ast * fh > cur) {
        buf->lpVtbl->Unlock(buf);
        goto done;
    }
    {
        // fit and box-filter straight from the locked frame
        if (max_w < 1) max_w = 1;
        if (max_h < 1) max_h = 1;
        double s = (double)max_w / fw;
        if ((double)max_h / fh < s) s = (double)max_h / fh;
        if (s > 1.0) s = 1.0;
        int ow = (int)(fw * s + 0.5), oh = (int)(fh * s + 0.5);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        unsigned char* out = (unsigned char*)malloc((size_t)ow * oh * 4);
        if (out) {
            for (int oy = 0; oy < oh; oy++) {
                int y0 = (int)((long long)oy * fh / oh), y1 = (int)((long long)(oy + 1) * fh / oh);
                if (y1 <= y0) y1 = y0 + 1;
                for (int ox = 0; ox < ow; ox++) {
                    int x0 = (int)((long long)ox * fw / ow), x1 = (int)((long long)(ox + 1) * fw / ow);
                    if (x1 <= x0) x1 = x0 + 1;
                    unsigned sb = 0, sg = 0, sr = 0, n = 0;
                    for (int y = y0; y < y1; y++) {
                        // a negative stride means the rows are stored bottom-up
                        const BYTE* row = stride >= 0 ? p + (size_t)y * ast
                                                      : p + (size_t)(fh - 1 - y) * ast;
                        for (int x = x0; x < x1; x++) {
                            sb += row[x * 4];
                            sg += row[x * 4 + 1];
                            sr += row[x * 4 + 2];
                            n++;
                        }
                    }
                    unsigned char* o = out + ((size_t)oy * ow + ox) * 4;
                    o[0] = (unsigned char)(sb / n);
                    o[1] = (unsigned char)(sg / n);
                    o[2] = (unsigned char)(sr / n);
                    o[3] = 255;
                }
            }
            *out_bgra = out;
            *out_w = ow;
            *out_h = oh;
            ok = true;
        }
    }
    buf->lpVtbl->Unlock(buf);
done:
    if (buf) buf->lpVtbl->Release(buf);
    if (smp) smp->lpVtbl->Release(smp);
    if (got) got->lpVtbl->Release(got);
    if (want) want->lpVtbl->Release(want);
    if (rd) rd->lpVtbl->Release(rd);
    if (at) at->lpVtbl->Release(at);
    return ok;
}
