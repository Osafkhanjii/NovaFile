// exr.c - OpenEXR preview decoder for NovaFile.  Pure C, no libraries.
//
// Windows has no .exr codec, so every frame of a render / comp image
// sequence showed a blank icon. This file reads the EXR itself and builds
// a small sRGB preview that the thumbnail worker turns into an HBITMAP.
//
// What is inside (each part follows the OpenEXR reference implementation
// bit for bit, so the pixels match what Nuke / DJV / OpenEXR show):
//   - header + offset table parsing (scanline, tiled, multi-part part 0)
//   - zlib inflate (stored / fixed / dynamic Huffman blocks)
//   - RLE, ZIPS, ZIP  (+ the byte predictor and the even/odd interleave)
//   - PIZ   (Huffman + Haar wavelet + reverse LUT)
//   - PXR24 (24-bit float / 16-bit half byte planes, zlib)
//   - B44, B44A (4x4 half blocks)
//   - DWAA, DWAB (8x8 lossy DCT with Rec.709 CSC, RLE + zlib side data)
// Every read is bounds-checked: a truncated or hostile file returns false
// (the caller keeps the normal icon) and can never read outside the map.
//
// Pixels are box-averaged straight into the preview size while the
// chunks are decoded, so a 4K frame never needs a full-size float copy.
// For DWA previews that shrink by 8x or more only the DC coefficient of
// each 8x8 block is used (the block average) - several times faster and
// visually identical at thumbnail size.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "exr.h"

// ------------------------------------------------------------ constants
enum { PT_UINT = 0, PT_HALF = 1, PT_FLOAT = 2 };
enum {
    C_NONE = 0, C_RLE = 1, C_ZIPS = 2, C_ZIP = 3, C_PIZ = 4, C_PXR24 = 5,
    C_B44 = 6, C_B44A = 7, C_DWAA = 8, C_DWAB = 9
};
#define EXR_MAX_CH      512
#define EXR_MAX_DIM     (1 << 20)           // a side longer than this is junk
#define EXR_MAX_CHUNK   ((size_t)1 << 30)   // decoded bytes of one chunk

typedef struct {
    char name[256];
    int  type, plin, xs, ys;
} XChan;

typedef struct {
    XChan ch[EXR_MAX_CH];
    int   nch;
    int   comp;
    int   dw[4], disp[4];           // x0 y0 x1 y1
    bool  has_dw, has_disp, has_ch;
    float par;
    int   tw, th, tmode;
    bool  has_tiles;
    int   chunk_count;
    bool  has_cc;
    char  type[64];
} XHdr;

typedef struct { size_t off; int nx, ny; } XPlane;

// ------------------------------------------------------------ small helpers
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static int floordiv(int a, int b) {           // b > 0
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}
static int modp(int a, int b) { return a - floordiv(a, b) * b; }
// How many v in [a, b] satisfy v % s == 0 (the EXR sampling rule).
static int num_samples(int s, int a, int b) {
    if (b < a) return 0;
    return floordiv(b, s) - floordiv(a - 1, s);
}
static int tsize(int type) { return type == PT_HALF ? 2 : 4; }

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static bool str_ieq(const char* a, const char* b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return false;
        a++; b++;
    }
    return *a == *b;
}

bool exr_is_exr_name(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    return n >= 4 && str_ieq(name + n - 4, ".exr");
}

// ------------------------------------------------------------ half floats
static float    g_h2f[65536];        // half bits -> float
static float    g_h2f_safe[65536];   // same, NaN -> 0 and +-inf -> +-65504
static uint16_t g_dwa_lin[65536];    // DWA nonlinear half -> linear half
static uint8_t  g_srgb[4097];        // linear [0,1] * 4096 -> sRGB byte
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static float h2f_calc(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16;
    int e = (h >> 10) & 0x1f;
    uint32_t m = h & 0x3ff, f;
    if (e == 0) {
        if (m == 0) {
            f = s;
        } else {
            while (!(m & 0x400)) { m <<= 1; e -= 1; }
            e += 1;
            m &= ~0x400u;
            f = s | ((uint32_t)(e + 112) << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = s | 0x7f800000u | (m << 13);
    } else {
        f = s | ((uint32_t)(e + 112) << 23) | (m << 13);
    }
    float r;
    memcpy(&r, &f, 4);
    return r;
}

// float -> half, round to nearest even (what OpenEXR's half(float) does).
static uint16_t f2h(float v) {
    uint32_t x;
    memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int fe = (int)((x >> 23) & 0xff);
    uint32_t m = x & 0x7fffff;
    if (fe == 0xff) return (uint16_t)(sign | 0x7c00 | (m ? 0x200 : 0));
    int e = fe - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7c00);
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        m |= 0x800000;
        int shift = 14 - e;
        uint32_t hm = m >> shift;
        uint32_t rem = m & ((1u << shift) - 1);
        uint32_t half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(sign | hm);
    }
    uint32_t out = sign | ((uint32_t)e << 10) | (m >> 13);
    uint32_t rem = m & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (out & 1))) out++;
    return (uint16_t)out;
}

static BOOL CALLBACK exr_init_tables(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o; (void)p; (void)c;
    for (int i = 0; i < 65536; i++) {
        g_h2f[i] = h2f_calc((uint16_t)i);
        g_h2f_safe[i] = g_h2f[i];
        if ((i & 0x7c00) == 0x7c00)
            g_h2f_safe[i] = (i & 0x3ff) ? 0.0f : (i & 0x8000) ? -65504.0f : 65504.0f;
    }
    // DWA stores perceptual values: |x| <= 1 is x^(1/2.2), above that a
    // log curve. This is the decoder's inverse (dwaCompressorToLinear).
    for (int i = 0; i < 65536; i++) {
        if ((i & 0x7c00) == 0x7c00) { g_dwa_lin[i] = 0; continue; }
        float f = g_h2f[i];
        float sign = f < 0 ? -1.0f : 1.0f;
        f = fabsf(f);
        f = (f <= 1.0f) ? powf(f, 2.2f) : expf(2.2f * (f - 1.0f));
        g_dwa_lin[i] = f2h(sign * f);
    }
    for (int i = 0; i <= 4096; i++) {
        double v = i / 4096.0;
        double s = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
        int b = (int)(s * 255.0 + 0.5);
        g_srgb[i] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
    return TRUE;
}

static void exr_tables(void) { InitOnceExecuteOnce(&g_once, exr_init_tables, NULL, NULL); }

// ------------------------------------------------------------ zlib inflate
typedef struct {
    uint16_t fast[512];
    uint16_t firstcode[16];
    int      maxcode[17];
    uint16_t firstsym[16];
    uint8_t  size[288];
    uint16_t value[288];
} ZHuf;

typedef struct {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t buf;
    int nb, over;
    uint8_t* out;
    size_t on, cap;
    ZHuf lit, dist;
} ZS;

static int zrev16(int n) {
    n = ((n & 0xAAAA) >> 1) | ((n & 0x5555) << 1);
    n = ((n & 0xCCCC) >> 2) | ((n & 0x3333) << 2);
    n = ((n & 0xF0F0) >> 4) | ((n & 0x0F0F) << 4);
    n = ((n & 0xFF00) >> 8) | ((n & 0x00FF) << 8);
    return n;
}

static bool zbuild(ZHuf* z, const uint8_t* sl, int num) {
    int sizes[17], next[16], code = 0, k = 0;
    memset(sizes, 0, sizeof sizes);
    memset(z->fast, 0, sizeof z->fast);
    for (int i = 0; i < num; i++) sizes[sl[i]]++;
    sizes[0] = 0;
    for (int i = 1; i < 16; i++)
        if (sizes[i] > (1 << i)) return false;
    for (int i = 1; i < 16; i++) {
        next[i] = code;
        z->firstcode[i] = (uint16_t)code;
        z->firstsym[i] = (uint16_t)k;
        code += sizes[i];
        if (sizes[i] && code - 1 >= (1 << i)) return false;
        z->maxcode[i] = code << (16 - i);
        code <<= 1;
        k += sizes[i];
    }
    z->maxcode[16] = 0x10000;
    for (int i = 0; i < num; i++) {
        int s = sl[i];
        if (!s) continue;
        int c = next[s] - z->firstcode[s] + z->firstsym[s];
        if (c < 0 || c >= 288) return false;
        z->size[c] = (uint8_t)s;
        z->value[c] = (uint16_t)i;
        if (s <= 9) {
            int j = zrev16(next[s]) >> (16 - s);
            while (j < 512) {
                z->fast[j] = (uint16_t)((s << 9) | i);
                j += 1 << s;
            }
        }
        next[s]++;
    }
    return true;
}

static void zfill(ZS* s) {
    while (s->nb <= 24) {
        uint32_t b = 0;
        if (s->p < s->end) b = *s->p++;
        else s->over++;
        s->buf |= b << s->nb;
        s->nb += 8;
    }
}
static uint32_t zbits(ZS* s, int n) {
    if (s->nb < n) zfill(s);
    uint32_t v = s->buf & ((1u << n) - 1);
    s->buf >>= n;
    s->nb -= n;
    return v;
}
static int zdecode(ZS* s, const ZHuf* z) {
    if (s->nb < 16) zfill(s);
    int b = z->fast[s->buf & 511];
    if (b) {
        int l = b >> 9;
        s->buf >>= l;
        s->nb -= l;
        return b & 511;
    }
    int k = zrev16((int)(s->buf & 0xffff));
    int l;
    for (l = 10;; l++)
        if (k < z->maxcode[l]) break;
    if (l >= 16) return -1;
    int c = (k >> (16 - l)) - z->firstcode[l] + z->firstsym[l];
    if (c < 0 || c >= 288 || z->size[c] != l) return -1;
    s->buf >>= l;
    s->nb -= l;
    return z->value[c];
}

static const int zlbase[31] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
static const int zlext[31] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                              3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0};
static const int zdbase[32] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                               257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                               8193, 12289, 16385, 24577, 0, 0};
static const int zdext[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                              7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 0, 0};

static bool zblock(ZS* s) {
    for (;;) {
        if (s->over > 8) return false;              // ran far past the input
        int z = zdecode(s, &s->lit);
        if (z < 0) return false;
        if (z < 256) {
            if (s->on >= s->cap) return false;
            s->out[s->on++] = (uint8_t)z;
        } else if (z == 256) {
            return true;
        } else {
            z -= 257;
            if (z >= 29) return false;
            int len = zlbase[z];
            if (zlext[z]) len += (int)zbits(s, zlext[z]);
            int d = zdecode(s, &s->dist);
            if (d < 0 || d >= 30) return false;
            int dist = zdbase[d];
            if (zdext[d]) dist += (int)zbits(s, zdext[d]);
            if ((size_t)dist > s->on || s->on + (size_t)len > s->cap) return false;
            uint8_t* o = s->out + s->on;
            const uint8_t* q = o - dist;
            for (int i = 0; i < len; i++) o[i] = q[i];
            s->on += (size_t)len;
        }
    }
}

static bool zdynamic(ZS* s) {
    static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int hlit = (int)zbits(s, 5) + 257;
    int hdist = (int)zbits(s, 5) + 1;
    int hclen = (int)zbits(s, 4) + 4;
    uint8_t cl[19];
    memset(cl, 0, sizeof cl);
    for (int i = 0; i < hclen; i++) cl[ord[i]] = (uint8_t)zbits(s, 3);
    ZHuf z;
    if (!zbuild(&z, cl, 19)) return false;
    uint8_t lens[288 + 32];
    int n = 0, ntot = hlit + hdist;
    while (n < ntot) {
        if (s->over > 8) return false;
        int c = zdecode(s, &z);
        if (c < 0 || c >= 19) return false;
        if (c < 16) {
            lens[n++] = (uint8_t)c;
        } else {
            uint8_t fill = 0;
            int rep;
            if (c == 16) {
                rep = (int)zbits(s, 2) + 3;
                if (n == 0) return false;
                fill = lens[n - 1];
            } else if (c == 17) {
                rep = (int)zbits(s, 3) + 3;
            } else {
                rep = (int)zbits(s, 7) + 11;
            }
            if (ntot - n < rep) return false;
            memset(lens + n, fill, (size_t)rep);
            n += rep;
        }
    }
    return zbuild(&s->lit, lens, hlit) && zbuild(&s->dist, lens + hlit, hdist);
}

// zlib stream -> out. Returns the byte count, or -1.
static long long zinflate(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    if (n < 2) return -1;
    int cmf = in[0], flg = in[1];
    if (((cmf << 8) | flg) % 31 || (cmf & 15) != 8 || (flg & 32)) return -1;
    ZS* s = (ZS*)malloc(sizeof(ZS));
    if (!s) return -1;
    memset(s, 0, sizeof *s);
    s->p = in + 2;
    s->end = in + n;
    s->out = out;
    s->cap = cap;
    bool ok = true;
    int final = 0;
    while (ok && !final) {
        final = (int)zbits(s, 1);
        int type = (int)zbits(s, 2);
        if (type == 0) {
            if (s->nb & 7) zbits(s, s->nb & 7);
            uint8_t hd[4];
            int k = 0;
            while (s->nb > 0 && k < 4) {
                hd[k++] = (uint8_t)(s->buf & 255);
                s->buf >>= 8;
                s->nb -= 8;
            }
            while (k < 4) {
                if (s->p >= s->end) { ok = false; break; }
                hd[k++] = *s->p++;
            }
            if (!ok) break;
            int len = hd[0] | (hd[1] << 8), nlen = hd[2] | (hd[3] << 8);
            if (nlen != (len ^ 0xffff) || s->nb != 0) { ok = false; break; }
            if ((size_t)(s->end - s->p) < (size_t)len || s->on + (size_t)len > s->cap) {
                ok = false;
                break;
            }
            memcpy(s->out + s->on, s->p, (size_t)len);
            s->p += len;
            s->on += (size_t)len;
        } else if (type == 1) {
            uint8_t l[288 + 32];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            for (int i = 0; i < 32; i++) l[288 + i] = 5;
            ok = zbuild(&s->lit, l, 288) && zbuild(&s->dist, l + 288, 32) && zblock(s);
        } else if (type == 2) {
            ok = zdynamic(s) && zblock(s);
        } else {
            ok = false;
        }
    }
    long long r = ok ? (long long)s->on : -1;
    free(s);
    return r;
}

// ------------------------------------------------------------ RLE / ZIP glue
static long long rle_unc(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        int c = (int8_t)in[i++];
        if (c < 0) {
            size_t cnt = (size_t)(-c);
            if (i + cnt > n || o + cnt > cap) return -1;
            memcpy(out + o, in + i, cnt);
            i += cnt;
            o += cnt;
        } else {
            size_t cnt = (size_t)c + 1;
            if (i >= n || o + cnt > cap) return -1;
            memset(out + o, in[i], cnt);
            i++;
            o += cnt;
        }
    }
    return (long long)o;
}

// The byte predictor + even/odd split every ZIP / RLE block is stored with.
static void unpredict_interleave(uint8_t* t, size_t n, uint8_t* out) {
    for (size_t i = 1; i < n; i++) t[i] = (uint8_t)(t[i - 1] + t[i] - 128);
    const uint8_t* t1 = t;
    const uint8_t* t2 = t + (n + 1) / 2;
    for (size_t i = 0; i < n; i++) out[i] = (i & 1) ? *t2++ : *t1++;
}

// ------------------------------------------------------------ decode context
#define HUF_ENCBITS 16
#define HUF_DECBITS 14
#define HUF_ENCSIZE ((1 << HUF_ENCBITS) + 1)
#define HUF_DECSIZE (1 << HUF_DECBITS)
#define HUF_DECMASK (HUF_DECSIZE - 1)
#define SHORT_ZEROCODE_RUN 59
#define LONG_ZEROCODE_RUN  63
#define SHORTEST_LONG_RUN  (2 + LONG_ZEROCODE_RUN - SHORT_ZEROCODE_RUN)

typedef struct {
    uint64_t hcode[HUF_ENCSIZE];
    int32_t  next[HUF_ENCSIZE];
    int32_t  lit[HUF_DECSIZE];
    int32_t  head[HUF_DECSIZE];
    int32_t  nlong[HUF_DECSIZE];
    uint8_t  len[HUF_DECSIZE];
} HufCtx;

typedef struct {
    HufCtx*   huf;           // lazily allocated (PIZ / DWA only)
    uint16_t* lut;           // PIZ reverse LUT, 65536 entries
    uint8_t*  tmp;  size_t tmp_cap;    // codec scratch
    uint8_t*  tmp2; size_t tmp2_cap;   // second scratch (DWA)
    uint8_t*  tmp3; size_t tmp3_cap;   // third scratch (DWA)
    int*      rowc;          // per-channel row counters (lines -> planar)
    bool      dc_only;       // DWA: use only the 8x8 block averages
    bool      dc_sparse;     // ...and write only each block's top-left
                             // pixel (accumulate_dc reads just those)
} XDec;

static uint8_t* grow(uint8_t** p, size_t* cap, size_t need) {
    if (need == 0) need = 1;
    if (*cap >= need) return *p;
    uint8_t* n = (uint8_t*)realloc(*p, need);
    if (!n) return NULL;
    *p = n;
    *cap = need;
    return n;
}

// Interleaved scanline layout (y, then every channel) -> planar planes.
static bool lines_to_planar(const XHdr* h, XDec* D, const XPlane* pl, int y0, int y1,
                            const uint8_t* src, size_t srclen, uint8_t* dst) {
    size_t pos = 0;
    for (int c = 0; c < h->nch; c++) D->rowc[c] = 0;
    for (int y = y0; y <= y1; y++) {
        for (int c = 0; c < h->nch; c++) {
            if (modp(y, h->ch[c].ys)) continue;
            size_t n = (size_t)pl[c].nx * (size_t)tsize(h->ch[c].type);
            if (pos + n > srclen) return false;
            memcpy(dst + pl[c].off + (size_t)D->rowc[c] * n, src + pos, n);
            pos += n;
            D->rowc[c]++;
        }
    }
    return pos == srclen;
}

// ------------------------------------------------------------ Huffman (PIZ, DWA AC)
static void huf_canonical(uint64_t* hcode, int im, int iM) {
    uint64_t n[59];
    memset(n, 0, sizeof n);
    for (int i = im; i <= iM; i++) n[hcode[i]] += 1;
    uint64_t c = 0;
    for (int i = 58; i > 0; --i) {
        uint64_t nc = (c + n[i]) >> 1;
        n[i] = c;
        c = nc;
    }
    for (int i = im; i <= iM; i++) {
        int l = (int)hcode[i];
        if (l > 0) hcode[i] = (uint64_t)l | (n[l]++ << 6);
    }
}

static bool huf_unpack(HufCtx* H, const uint8_t** pp, const uint8_t* end, int im, int iM) {
    const uint8_t* p = *pp;
    uint64_t c = 0;
    int lc = 0;
    int im0 = im;
    for (int i = im; i <= iM; i++) H->hcode[i] = 0;
    for (; im <= iM; im++) {
        while (lc < 6) {
            if (p >= end) return false;
            c = (c << 8) | *p++;
            lc += 8;
        }
        lc -= 6;
        uint64_t l = (c >> lc) & 63;
        H->hcode[im] = l;
        if (l == LONG_ZEROCODE_RUN) {
            while (lc < 8) {
                if (p >= end) return false;
                c = (c << 8) | *p++;
                lc += 8;
            }
            lc -= 8;
            int zerun = (int)((c >> lc) & 0xff) + SHORTEST_LONG_RUN;
            if (im + zerun > iM + 1) return false;
            while (zerun--) H->hcode[im++] = 0;
            im--;
        } else if (l >= SHORT_ZEROCODE_RUN) {
            int zerun = (int)l - SHORT_ZEROCODE_RUN + 2;
            if (im + zerun > iM + 1) return false;
            while (zerun--) H->hcode[im++] = 0;
            im--;
        }
    }
    *pp = p;
    huf_canonical(H->hcode, im0, iM);
    return true;
}

static bool huf_build(HufCtx* H, int im, int iM) {
    memset(H->len, 0, sizeof H->len);
    memset(H->nlong, 0, sizeof H->nlong);
    memset(H->head, 0xff, sizeof H->head);
    for (int i = im; i <= iM; i++) {
        uint64_t c = H->hcode[i] >> 6;
        int l = (int)(H->hcode[i] & 63);
        if (c >> l) return false;
        if (l > HUF_DECBITS) {
            uint64_t idx = c >> (l - HUF_DECBITS);
            if (idx >= HUF_DECSIZE || H->len[idx]) return false;
            H->nlong[idx]++;
            H->next[i] = H->head[idx];
            H->head[idx] = i;
        } else if (l) {
            uint64_t idx = c << (HUF_DECBITS - l);
            uint64_t cnt = 1ull << (HUF_DECBITS - l);
            if (idx + cnt > HUF_DECSIZE) return false;
            for (uint64_t k = 0; k < cnt; k++) {
                if (H->len[idx + k] || H->nlong[idx + k]) return false;
                H->len[idx + k] = (uint8_t)l;
                H->lit[idx + k] = i;
            }
        }
    }
    return true;
}

// Hot loop of every PIZ / DWA decode: state lives in locals (registers),
// the code emitter is a macro. Same bit-exact logic as OpenEXR hufDecode.
#define HUF_GETCODE(po)                                                  \
    do {                                                                 \
        int po_ = (po);                                                  \
        if (po_ == rlc) {                                                \
            if (lc < 8) {                                                \
                c = (c << 8) | (p < ie ? *p : 0);                        \
                p++;                                                     \
                lc += 8;                                                 \
            }                                                            \
            lc -= 8;                                                     \
            unsigned cs_ = (unsigned)((c >> lc) & 0xff);                 \
            if (o + cs_ > no || o == 0) return false;                    \
            uint16_t v_ = out[o - 1];                                    \
            while (cs_-- > 0) out[o++] = v_;                             \
        } else {                                                         \
            if (o >= no) return false;                                   \
            out[o++] = (uint16_t)po_;                                    \
        }                                                                \
    } while (0)

static bool huf_decode(const HufCtx* H, const uint8_t* in, long long nbits, int rlc,
                       uint16_t* out, size_t no) {
    uint64_t c = 0;
    int lc = 0;
    const uint8_t* p = in;
    const uint8_t* ie = in + (nbits + 7) / 8;
    size_t o = 0;
    const uint8_t* len = H->len;
    const int32_t* lit = H->lit;
    while (p < ie) {
        c = (c << 8) | *p++;
        lc += 8;
        while (lc >= HUF_DECBITS) {
            int idx = (int)((c >> (lc - HUF_DECBITS)) & HUF_DECMASK);
            int l = len[idx];
            if (l) {
                lc -= l;
                HUF_GETCODE(lit[idx]);
            } else {
                if (!H->nlong[idx]) return false;
                bool found = false;
                for (int j = H->head[idx]; j >= 0; j = H->next[j]) {
                    int ll = (int)(H->hcode[j] & 63);
                    while (lc < ll && p < ie) {
                        c = (c << 8) | *p++;
                        lc += 8;
                    }
                    if (lc >= ll && ll < 64 &&
                        (H->hcode[j] >> 6) == ((c >> (lc - ll)) & ((1ull << ll) - 1))) {
                        lc -= ll;
                        HUF_GETCODE(j);
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        }
    }
    int i = (int)((8 - nbits) & 7);
    c >>= i;
    lc -= i;
    if (lc < 0) return false;
    while (lc > 0) {
        int idx = (int)((c << (HUF_DECBITS - lc)) & HUF_DECMASK);
        if (!len[idx]) return false;
        lc -= len[idx];
        if (lc < 0) return false;
        HUF_GETCODE(lit[idx]);
    }
    return o == no;
}
#undef HUF_GETCODE

static bool huf_uncompress(XDec* D, const uint8_t* in, size_t n, uint16_t* out, size_t nout) {
    if (n == 0) return nout == 0;
    if (n < 20) return false;
    if (!D->huf) {
        D->huf = (HufCtx*)malloc(sizeof(HufCtx));
        if (!D->huf) return false;
    }
    uint32_t im = rd32(in), iM = rd32(in + 4), nbits = rd32(in + 12);
    if (im >= HUF_ENCSIZE || iM >= HUF_ENCSIZE || im > iM) return false;
    const uint8_t* p = in + 20;
    if (!huf_unpack(D->huf, &p, in + n, (int)im, (int)iM)) return false;
    if ((uint64_t)nbits > 8ull * (uint64_t)(n - (size_t)(p - in))) return false;
    if (!huf_build(D->huf, (int)im, (int)iM)) return false;
    return huf_decode(D->huf, p, (long long)nbits, (int)iM, out, nout);
}

// ------------------------------------------------------------ PIZ
static void wdec14(uint16_t l, uint16_t h, uint16_t* a, uint16_t* b) {
    int16_t ls = (int16_t)l, hs = (int16_t)h;
    int hi = hs;
    int ai = ls + (hi & 1) + (hi >> 1);
    *a = (uint16_t)(int16_t)ai;
    *b = (uint16_t)(int16_t)(ai - hi);
}
static void wdec16(uint16_t l, uint16_t h, uint16_t* a, uint16_t* b) {
    int m = l, d = h;
    int bb = (m - (d >> 1)) & 0xffff;
    int aa = (d + bb - 0x8000) & 0xffff;
    *b = (uint16_t)bb;
    *a = (uint16_t)aa;
}

static void wav2_decode(uint16_t* in, int nx, int ox, int ny, int oy, uint16_t mx) {
    bool w14 = mx < (1 << 14);
    int n = nx > ny ? ny : nx;
    int p = 1, p2;
    while (p <= n) p <<= 1;
    p >>= 1;
    p2 = p;
    p >>= 1;
    while (p >= 1) {
        uint16_t* py = in;
        uint16_t* ey = in + (ptrdiff_t)oy * (ny - p2);
        int oy1 = oy * p, oy2 = oy * p2, ox1 = ox * p, ox2 = ox * p2;
        uint16_t i00, i01, i10, i11;
        for (; py <= ey; py += oy2) {
            uint16_t* px = py;
            uint16_t* ex = py + (ptrdiff_t)ox * (nx - p2);
            for (; px <= ex; px += ox2) {
                uint16_t* p01 = px + ox1;
                uint16_t* p10 = px + oy1;
                uint16_t* p11 = p10 + ox1;
                if (w14) {
                    wdec14(*px, *p10, &i00, &i10);
                    wdec14(*p01, *p11, &i01, &i11);
                    wdec14(i00, i01, px, p01);
                    wdec14(i10, i11, p10, p11);
                } else {
                    wdec16(*px, *p10, &i00, &i10);
                    wdec16(*p01, *p11, &i01, &i11);
                    wdec16(i00, i01, px, p01);
                    wdec16(i10, i11, p10, p11);
                }
            }
            if (nx & p) {
                uint16_t* p10 = px + oy1;
                if (w14) wdec14(*px, *p10, &i00, p10);
                else wdec16(*px, *p10, &i00, p10);
                *px = i00;
            }
        }
        if (ny & p) {
            uint16_t* px = py;
            uint16_t* ex = py + (ptrdiff_t)ox * (nx - p2);
            for (; px <= ex; px += ox2) {
                uint16_t* p01 = px + ox1;
                if (w14) wdec14(*px, *p01, &i00, p01);
                else wdec16(*px, *p01, &i00, p01);
                *px = i00;
            }
        }
        p2 = p;
        p >>= 1;
    }
}

static bool piz_decode(const XHdr* h, XDec* D, const uint8_t* in, size_t n,
                       const XPlane* pl, uint8_t* out, size_t raw) {
    if (n < 4 || (raw & 1)) return false;
    const uint8_t* e = in + n;
    uint16_t minnz = rd16(in), maxnz = rd16(in + 2);
    const uint8_t* p = in + 4;
    if (maxnz >= 8192) return false;
    uint8_t bitmap[8192];
    memset(bitmap, 0, sizeof bitmap);
    if (minnz <= maxnz) {
        size_t k = (size_t)maxnz - minnz + 1;
        if ((size_t)(e - p) < k) return false;
        memcpy(bitmap + minnz, p, k);
        p += k;
    }
    if (!D->lut) {
        D->lut = (uint16_t*)malloc(65536 * sizeof(uint16_t));
        if (!D->lut) return false;
    }
    int k = 0;
    for (int i = 0; i < 65536; i++)
        if (i == 0 || (bitmap[i >> 3] & (1 << (i & 7)))) D->lut[k++] = (uint16_t)i;
    uint16_t maxv = (uint16_t)(k - 1);
    while (k < 65536) D->lut[k++] = 0;
    if (e - p < 4) return false;
    uint32_t len = rd32(p);
    p += 4;
    if (len > (size_t)(e - p)) return false;
    uint16_t* tmp = (uint16_t*)out;
    size_t cnt = raw / 2;
    if (!huf_uncompress(D, p, len, tmp, cnt)) return false;
    for (int c = 0; c < h->nch; c++) {
        int s16 = tsize(h->ch[c].type) / 2;
        uint16_t* base = (uint16_t*)(out + pl[c].off);
        if (pl[c].nx < 1 || pl[c].ny < 1) continue;
        for (int j = 0; j < s16; j++)
            wav2_decode(base + j, pl[c].nx, s16, pl[c].ny, pl[c].nx * s16, maxv);
    }
    for (size_t i = 0; i < cnt; i++) tmp[i] = D->lut[tmp[i]];
    return true;
}

// ------------------------------------------------------------ PXR24
static bool pxr24_decode(const XHdr* h, XDec* D, const uint8_t* in, size_t n,
                         const XPlane* pl, int y0, int y1, uint8_t* out, size_t raw) {
    uint8_t* t = grow(&D->tmp, &D->tmp_cap, raw);
    if (!t) return false;
    long long got = zinflate(in, n, t, raw);
    if (got < 0) return false;
    const uint8_t* q = t;
    const uint8_t* qe = t + got;
    for (int c = 0; c < h->nch; c++) D->rowc[c] = 0;
    for (int y = y0; y <= y1; y++) {
        for (int c = 0; c < h->nch; c++) {
            if (modp(y, h->ch[c].ys)) continue;
            int nx = pl[c].nx;
            int ts = tsize(h->ch[c].type);
            uint8_t* dst = out + pl[c].off + (size_t)D->rowc[c] * (size_t)nx * ts;
            D->rowc[c]++;
            if (h->ch[c].type == PT_HALF) {
                if (qe - q < 2 * (ptrdiff_t)nx) return false;
                const uint8_t *p0 = q, *p1 = q + nx;
                q += 2 * (size_t)nx;
                uint16_t pix = 0;
                for (int j = 0; j < nx; j++) {
                    pix = (uint16_t)(pix + (uint16_t)((p0[j] << 8) | p1[j]));
                    dst[2 * j] = (uint8_t)pix;
                    dst[2 * j + 1] = (uint8_t)(pix >> 8);
                }
            } else if (h->ch[c].type == PT_UINT) {
                if (qe - q < 4 * (ptrdiff_t)nx) return false;
                const uint8_t *p0 = q, *p1 = q + nx, *p2 = q + 2 * nx, *p3 = q + 3 * nx;
                q += 4 * (size_t)nx;
                uint32_t pix = 0;
                for (int j = 0; j < nx; j++) {
                    pix += ((uint32_t)p0[j] << 24) | ((uint32_t)p1[j] << 16) |
                           ((uint32_t)p2[j] << 8) | p3[j];
                    memcpy(dst + 4 * j, &pix, 4);
                }
            } else {
                if (qe - q < 3 * (ptrdiff_t)nx) return false;
                const uint8_t *p0 = q, *p1 = q + nx, *p2 = q + 2 * nx;
                q += 3 * (size_t)nx;
                uint32_t pix = 0;
                for (int j = 0; j < nx; j++) {
                    pix += ((uint32_t)p0[j] << 24) | ((uint32_t)p1[j] << 16) |
                           ((uint32_t)p2[j] << 8);
                    memcpy(dst + 4 * j, &pix, 4);
                }
            }
        }
    }
    return q == qe;
}

// ------------------------------------------------------------ B44 / B44A
static void unpack14(const uint8_t b[14], uint16_t s[16]) {
    s[0] = (uint16_t)((b[0] << 8) | b[1]);
    uint16_t shift = (uint16_t)(b[2] >> 2);
    uint16_t bias = (uint16_t)(0x20 << shift);
#define B44V(prev, bits) (uint16_t)((prev) + ((bits) << shift) - bias)
    s[4]  = B44V(s[0],  ((((b[2] << 4) | (b[3] >> 4)) & 0x3f)));
    s[8]  = B44V(s[4],  ((((b[3] << 2) | (b[4] >> 6)) & 0x3f)));
    s[12] = B44V(s[8],  (b[4] & 0x3f));
    s[1]  = B44V(s[0],  (b[5] >> 2));
    s[5]  = B44V(s[4],  ((((b[5] << 4) | (b[6] >> 4)) & 0x3f)));
    s[9]  = B44V(s[8],  ((((b[6] << 2) | (b[7] >> 6)) & 0x3f)));
    s[13] = B44V(s[12], (b[7] & 0x3f));
    s[2]  = B44V(s[1],  (b[8] >> 2));
    s[6]  = B44V(s[5],  ((((b[8] << 4) | (b[9] >> 4)) & 0x3f)));
    s[10] = B44V(s[9],  ((((b[9] << 2) | (b[10] >> 6)) & 0x3f)));
    s[14] = B44V(s[13], (b[10] & 0x3f));
    s[3]  = B44V(s[2],  (b[11] >> 2));
    s[7]  = B44V(s[6],  ((((b[11] << 4) | (b[12] >> 4)) & 0x3f)));
    s[11] = B44V(s[10], ((((b[12] << 2) | (b[13] >> 6)) & 0x3f)));
    s[15] = B44V(s[14], (b[13] & 0x3f));
#undef B44V
    for (int i = 0; i < 16; i++) {
        if (s[i] & 0x8000) s[i] &= 0x7fff;
        else s[i] = (uint16_t)~s[i];
    }
}
static void unpack3(const uint8_t b[3], uint16_t s[16]) {
    s[0] = (uint16_t)((b[0] << 8) | b[1]);
    if (s[0] & 0x8000) s[0] &= 0x7fff;
    else s[0] = (uint16_t)~s[0];
    for (int i = 1; i < 16; i++) s[i] = s[0];
}

static bool b44_decode(const XHdr* h, const uint8_t* in, size_t n, const XPlane* pl,
                       uint8_t* out) {
    const uint8_t* p = in;
    const uint8_t* e = in + n;
    for (int c = 0; c < h->nch; c++) {
        int nx = pl[c].nx, ny = pl[c].ny;
        if (h->ch[c].type != PT_HALF) {
            size_t bytes = (size_t)nx * ny * tsize(h->ch[c].type);
            if ((size_t)(e - p) < bytes) return false;
            memcpy(out + pl[c].off, p, bytes);
            p += bytes;
            continue;
        }
        uint16_t* base = (uint16_t*)(out + pl[c].off);
        for (int y = 0; y < ny; y += 4) {
            uint16_t* r0 = base + (size_t)y * nx;
            uint16_t* r1 = r0 + nx;
            uint16_t* r2 = r1 + nx;
            uint16_t* r3 = r2 + nx;
            if (y + 3 >= ny) {
                if (y + 1 >= ny) r1 = r0;
                if (y + 2 >= ny) r2 = r1;
                r3 = r2;
            }
            for (int x = 0; x < nx; x += 4) {
                uint16_t s[16];
                if (e - p < 3) return false;
                if (p[2] >= (13 << 2)) {
                    unpack3(p, s);
                    p += 3;
                } else {
                    if (e - p < 14) return false;
                    unpack14(p, s);
                    p += 14;
                }
                int nn = (x + 3 < nx) ? 4 : nx - x;
                size_t nb = (size_t)nn * 2;
                if (y + 3 < ny) {
                    memcpy(r0, s, nb);
                    memcpy(r1, s + 4, nb);
                    memcpy(r2, s + 8, nb);
                    memcpy(r3, s + 12, nb);
                } else {
                    memcpy(r0, s, nb);
                    if (y + 1 < ny) memcpy(r1, s + 4, nb);
                    if (y + 2 < ny) memcpy(r2, s + 8, nb);
                }
                r0 += 4; r1 += 4; r2 += 4; r3 += 4;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------ DWAA / DWAB
enum { DWA_UNKNOWN = 0, DWA_LOSSY = 1, DWA_RLE = 2 };

typedef struct {
    char suf[256];
    int  csc, scheme, type;
    bool ci;
} DRule;

typedef struct {
    char prefix[256];
    int  idx[3];
} DSet;

static const uint8_t g_unzig[64] = {     // row-major position -> zig-zag index
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

static void idct8x8(float* data) {
    const float a = .5f * cosf(3.14159f / 4.0f);
    const float b = .5f * cosf(3.14159f / 16.0f);
    const float c = .5f * cosf(3.14159f / 8.0f);
    const float d = .5f * cosf(3.f * 3.14159f / 16.0f);
    const float e = .5f * cosf(5.f * 3.14159f / 16.0f);
    const float f = .5f * cosf(3.f * 3.14159f / 8.0f);
    const float g = .5f * cosf(7.f * 3.14159f / 16.0f);
    float al[4], be[4], th[4], ga[4];
    for (int row = 0; row < 8; ++row) {
        float* r = data + row * 8;
        al[0] = c * r[2];
        al[1] = f * r[2];
        al[2] = c * r[6];
        al[3] = f * r[6];
        be[0] = b * r[1] + d * r[3] + e * r[5] + g * r[7];
        be[1] = d * r[1] - g * r[3] - b * r[5] - e * r[7];
        be[2] = e * r[1] - b * r[3] + g * r[5] + d * r[7];
        be[3] = g * r[1] - e * r[3] + d * r[5] - b * r[7];
        th[0] = a * (r[0] + r[4]);
        th[3] = a * (r[0] - r[4]);
        th[1] = al[0] + al[3];
        th[2] = al[1] - al[2];
        ga[0] = th[0] + th[1];
        ga[1] = th[3] + th[2];
        ga[2] = th[3] - th[2];
        ga[3] = th[0] - th[1];
        r[0] = ga[0] + be[0];
        r[1] = ga[1] + be[1];
        r[2] = ga[2] + be[2];
        r[3] = ga[3] + be[3];
        r[4] = ga[3] - be[3];
        r[5] = ga[2] - be[2];
        r[6] = ga[1] - be[1];
        r[7] = ga[0] - be[0];
    }
    for (int col = 0; col < 8; ++col) {
        al[0] = c * data[16 + col];
        al[1] = f * data[16 + col];
        al[2] = c * data[48 + col];
        al[3] = f * data[48 + col];
        be[0] = b * data[8 + col] + d * data[24 + col] + e * data[40 + col] + g * data[56 + col];
        be[1] = d * data[8 + col] - g * data[24 + col] - b * data[40 + col] - e * data[56 + col];
        be[2] = e * data[8 + col] - b * data[24 + col] + g * data[40 + col] + d * data[56 + col];
        be[3] = g * data[8 + col] - e * data[24 + col] + d * data[40 + col] - b * data[56 + col];
        th[0] = a * (data[col] + data[32 + col]);
        th[3] = a * (data[col] - data[32 + col]);
        th[1] = al[0] + al[3];
        th[2] = al[1] - al[2];
        ga[0] = th[0] + th[1];
        ga[1] = th[3] + th[2];
        ga[2] = th[3] - th[2];
        ga[3] = th[0] - th[1];
        data[col] = ga[0] + be[0];
        data[8 + col] = ga[1] + be[1];
        data[16 + col] = ga[2] + be[2];
        data[24 + col] = ga[3] + be[3];
        data[32 + col] = ga[3] - be[3];
        data[40 + col] = ga[2] - be[2];
        data[48 + col] = ga[1] - be[1];
        data[56 + col] = ga[0] - be[0];
    }
}

// One CSC set (3 comps) or one plain channel (1 comp) of 8x8 DCT blocks.
static bool dwa_lossy(int ncomp, uint8_t* const planes[3], const int types[3],
                      int w, int h, const uint16_t** ac, const uint16_t* ac_end,
                      const uint16_t** dc, const uint16_t* dc_end, const bool lin[3],
                      int dc_mode) {
    // dc_mode: 0 = full decode, 1 = block averages written to every pixel,
    //          2 = block averages written to each block's top-left pixel only
    bool dc_only = dc_mode != 0;
    if (w < 1 || h < 1) return true;
    int nbx = (w + 7) / 8, nby = (h + 7) / 8;
    size_t nblk = (size_t)nbx * nby;
    if ((size_t)(dc_end - *dc) < nblk * (size_t)ncomp) return false;
    const uint16_t* dcp[3];
    for (int c = 0; c < ncomp; c++) dcp[c] = *dc + nblk * c;
    const uint16_t* acp = *ac;
    float blk[3][64];
    uint16_t zig[64];
    for (int by = 0; by < nby; by++) {
        int maxy = (by == nby - 1) ? h - by * 8 : 8;
        for (int bx = 0; bx < nbx; bx++) {
            int maxx = (bx == nbx - 1) ? w - bx * 8 : 8;
            bool constant = true;
            for (int c = 0; c < ncomp; c++) {
                if (dc_only) {
                    // thumbnail: the block average is all we need, and the
                    // AC stream was never even decompressed (dwa_decode)
                    float v = g_h2f[*dcp[c]++] * 3.535536e-01f * 3.535536e-01f;
                    blk[c][0] = v;
                    continue;
                }
                memset(zig, 0, sizeof zig);
                zig[0] = *dcp[c]++;
                int k = 1, last = 0;
                while (k < 64) {                    // un-RLE the AC run
                    if (acp >= ac_end) return false;
                    uint16_t v = *acp++;
                    if (v == 0xff00) {
                        k = 64;
                    } else if ((v >> 8) == 0xff) {
                        k += v & 0xff;
                    } else {
                        last = k;
                        zig[k] = v;
                        k++;
                    }
                }
                if (last == 0) {
                    float v = g_h2f[zig[0]] * 3.535536e-01f * 3.535536e-01f;
                    for (int i = 0; i < 64; i++) blk[c][i] = v;
                } else {
                    constant = false;
                    for (int i = 0; i < 64; i++) blk[c][i] = g_h2f[zig[g_unzig[i]]];
                    idct8x8(blk[c]);
                }
            }
            int npx = constant ? 1 : 64;
            if (ncomp == 3) {
                for (int i = 0; i < npx; i++) {
                    float y = blk[0][i], cb = blk[1][i], cr = blk[2][i];
                    blk[0][i] = y + 1.5747f * cr;
                    blk[1][i] = y - 0.1873f * cb - 0.4682f * cr;
                    blk[2][i] = y + 1.8556f * cb;
                }
            }
            for (int c = 0; c < ncomp; c++) {
                // a constant block converts its one value once, not 64 times
                uint16_t cv = f2h(blk[c][0]);
                if (lin[c]) cv = g_dwa_lin[cv];
                int ey = (dc_mode == 2) ? 1 : maxy;
                int ex = (dc_mode == 2) ? 1 : maxx;
                for (int yy = 0; yy < ey; yy++) {
                    size_t row = (size_t)(by * 8 + yy) * w + (size_t)bx * 8;
                    for (int xx = 0; xx < ex; xx++) {
                        uint16_t hv = cv;
                        if (!constant) {
                            hv = f2h(blk[c][yy * 8 + xx]);
                            if (lin[c]) hv = g_dwa_lin[hv];
                        }
                        if (types[c] == PT_HALF) {
                            memcpy(planes[c] + (row + xx) * 2, &hv, 2);
                        } else {
                            float fv = g_h2f[hv];
                            memcpy(planes[c] + (row + xx) * 4, &fv, 4);
                        }
                    }
                }
            }
        }
    }
    *ac = acp;
    *dc = *dc + nblk * ncomp;
    return true;
}

static void dwa_add_rule(DRule* r, int* n, const char* suf, int scheme, int type,
                         int csc, bool ci) {
    if (*n >= 64) return;
    DRule* d = &r[(*n)++];
    strncpy(d->suf, suf, sizeof d->suf - 1);
    d->suf[sizeof d->suf - 1] = 0;
    d->scheme = scheme;
    d->type = type;
    d->csc = csc;
    d->ci = ci;
}

static int dset_cmp(const void* a, const void* b) {
    return strcmp(((const DSet*)a)->prefix, ((const DSet*)b)->prefix);
}

static bool dwa_decode(const XHdr* h, XDec* D, const uint8_t* in, size_t n,
                       const XPlane* pl, uint8_t* out) {
    if (n < 88) return false;
    uint64_t cnt[11];
    for (int i = 0; i < 11; i++) cnt[i] = rd64(in + 8 * i);
    uint64_t version = cnt[0], unk_unc = cnt[1], unk_c = cnt[2], ac_c = cnt[3],
             dc_c = cnt[4], rle_c = cnt[5], rle_unc_sz = cnt[6], rle_raw = cnt[7],
             ac_total = cnt[8], dc_total = cnt[9], ac_comp = cnt[10];
    if (version > 2) return false;
    const uint8_t* p = in + 88;
    const uint8_t* e = in + n;

    DRule* rules = (DRule*)malloc(64 * sizeof(DRule));
    DSet* sets = (DSet*)malloc((size_t)h->nch * sizeof(DSet));
    int* scheme = (int*)malloc((size_t)h->nch * sizeof(int));
    bool ok = false;
    if (!rules || !sets || !scheme) goto done;
    int nr = 0;
    if (version < 2) {
        static const char* rs[] = {"r", "red", "g", "grn", "green", "b", "blu", "blue"};
        static const int rc[] = {0, 0, 1, 1, 1, 2, 2, 2};
        for (int i = 0; i < 8; i++) dwa_add_rule(rules, &nr, rs[i], DWA_LOSSY, PT_HALF, rc[i], true);
        dwa_add_rule(rules, &nr, "y", DWA_LOSSY, PT_HALF, -1, true);
        dwa_add_rule(rules, &nr, "by", DWA_LOSSY, PT_HALF, -1, true);
        dwa_add_rule(rules, &nr, "ry", DWA_LOSSY, PT_HALF, -1, true);
        dwa_add_rule(rules, &nr, "a", DWA_RLE, PT_UINT, -1, true);
        dwa_add_rule(rules, &nr, "a", DWA_RLE, PT_HALF, -1, true);
        dwa_add_rule(rules, &nr, "a", DWA_RLE, PT_FLOAT, -1, true);
    } else {
        if (e - p < 2) goto done;
        int rsz = rd16(p);
        if (rsz < 2 || e - p < rsz) goto done;
        const uint8_t* rp = p + 2;
        const uint8_t* re = p + rsz;
        p += rsz;
        while (rp < re) {
            DRule r;
            int len = 0;
            while (rp < re && *rp && len < 255) r.suf[len++] = (char)*rp++;
            if (rp >= re || *rp) goto done;
            r.suf[len] = 0;
            rp++;
            if (re - rp < 2) goto done;
            int v = rp[0];
            r.csc = (v >> 4) - 1;
            r.scheme = (v >> 2) & 3;
            r.ci = (v & 1) != 0;
            r.type = rp[1];
            rp += 2;
            if (r.csc < -1 || r.csc >= 3 || r.scheme > 2 || r.type > 2) goto done;
            if (nr < 64) rules[nr++] = r;
        }
    }
    if ((uint64_t)(e - p) < unk_c || (uint64_t)(e - p) - unk_c < ac_c ||
        (uint64_t)(e - p) - unk_c - ac_c < dc_c ||
        (uint64_t)(e - p) - unk_c - ac_c - dc_c < rle_c)
        goto done;
    const uint8_t* unk_buf = p;
    const uint8_t* ac_buf = unk_buf + unk_c;
    const uint8_t* dc_buf = ac_buf + ac_c;
    const uint8_t* rle_buf = dc_buf + dc_c;

    // classify every channel; collect RGB triples per layer prefix
    int nsets = 0;
    size_t unk_total = 0, rle_total = 0, lossy_blocks = 0;
    for (int c = 0; c < h->nch; c++) {
        const char* name = h->ch[c].name;
        const char* dot = strrchr(name, '.');
        const char* suf = dot ? dot + 1 : name;
        char prefix[256];
        size_t pn = dot ? (size_t)(dot - name) : 0;
        memcpy(prefix, name, pn);
        prefix[pn] = 0;
        int si = -1;
        for (int k = 0; k < nsets; k++)
            if (!strcmp(sets[k].prefix, prefix)) { si = k; break; }
        if (si < 0) {
            si = nsets++;
            memcpy(sets[si].prefix, prefix, pn + 1);
            sets[si].idx[0] = sets[si].idx[1] = sets[si].idx[2] = -1;
        }
        scheme[c] = DWA_UNKNOWN;
        for (int r = 0; r < nr; r++) {
            if (rules[r].type != h->ch[c].type) continue;
            if (rules[r].ci ? !str_ieq(suf, rules[r].suf) : strcmp(suf, rules[r].suf)) continue;
            scheme[c] = rules[r].scheme;
            if (rules[r].csc >= 0) sets[si].idx[rules[r].csc] = c;
        }
        size_t bytes = (size_t)pl[c].nx * pl[c].ny * tsize(h->ch[c].type);
        if (scheme[c] == DWA_UNKNOWN) unk_total += bytes;
        else if (scheme[c] == DWA_RLE) rle_total += bytes;
        else lossy_blocks += (size_t)((pl[c].nx + 7) / 8) * (size_t)((pl[c].ny + 7) / 8);
    }
    qsort(sets, (size_t)nsets, sizeof(DSet), dset_cmp);

    // side buffers
    if (unk_unc > unk_total || rle_raw > rle_total) goto done;
    // at most 63 AC values and exactly one DC value per 8x8 block
    if (ac_total > lossy_blocks * 63 || dc_total > lossy_blocks) goto done;
    if (rle_unc_sz > (uint64_t)rle_total * 2 + 64) goto done;
    size_t need1 = (size_t)unk_total + (size_t)rle_total;
    size_t ac_keep = D->dc_only ? 0 : (size_t)ac_total;  // DC-only: no AC
    size_t need2 = (ac_keep + (size_t)dc_total) * 2;
    uint8_t* b1 = grow(&D->tmp, &D->tmp_cap, need1 + (size_t)rle_unc_sz);
    uint8_t* b2 = grow(&D->tmp2, &D->tmp2_cap, need2);
    if (!b1 || !b2) goto done;
    uint8_t* unk_plane = b1;
    uint8_t* rle_plane = b1 + unk_total;
    uint8_t* rle_tmp = b1 + need1;
    uint16_t* acv = (uint16_t*)b2;
    uint16_t* dcv = acv + ac_keep;
    memset(unk_plane, 0, need1);

    if (unk_c > 0) {
        long long got = zinflate(unk_buf, (size_t)unk_c, unk_plane, (size_t)unk_unc);
        if (got != (long long)unk_unc) goto done;
    }
    // DC-only (a thumbnail 8x+ smaller than the frame): the AC stream - by
    // far the biggest and slowest part of a DWA chunk - is skipped whole.
    if (ac_c > 0 && !D->dc_only) {
        if (ac_comp == 0) {
            if (!huf_uncompress(D, ac_buf, (size_t)ac_c, acv, (size_t)ac_total)) goto done;
        } else if (ac_comp == 1) {
            long long got = zinflate(ac_buf, (size_t)ac_c, (uint8_t*)acv, (size_t)ac_total * 2);
            if (got != (long long)ac_total * 2) goto done;
        } else {
            goto done;
        }
    }
    if (dc_c > 0) {
        size_t dn = (size_t)dc_total * 2;
        uint8_t* t = grow(&D->tmp3, &D->tmp3_cap, dn);
        if (!t) goto done;
        long long got = zinflate(dc_buf, (size_t)dc_c, t, dn);
        if (got != (long long)dn) goto done;
        unpredict_interleave(t, dn, (uint8_t*)dcv);
    }
    if (rle_raw > 0) {
        long long got = zinflate(rle_buf, (size_t)rle_c, rle_tmp, (size_t)rle_unc_sz);
        if (got != (long long)rle_unc_sz) goto done;
        if (rle_unc(rle_tmp, (size_t)rle_unc_sz, rle_plane, (size_t)rle_raw) != (long long)rle_raw)
            goto done;
    }

    const uint16_t* acp = acv;
    const uint16_t* ace = acv + ac_keep;
    const uint16_t* dcp = dcv;
    const uint16_t* dce = dcv + dc_total;
    bool* done_ch = (bool*)calloc((size_t)h->nch, sizeof(bool));
    if (!done_ch) goto done;
    bool fine = true;
    for (int s = 0; s < nsets && fine; s++) {
        int r = sets[s].idx[0], g = sets[s].idx[1], b = sets[s].idx[2];
        if (r < 0 || g < 0 || b < 0) continue;
        if (h->ch[r].xs != h->ch[g].xs || h->ch[r].xs != h->ch[b].xs ||
            h->ch[r].ys != h->ch[g].ys || h->ch[r].ys != h->ch[b].ys)
            continue;
        if (scheme[r] != DWA_LOSSY || scheme[g] != DWA_LOSSY || scheme[b] != DWA_LOSSY) {
            fine = false;
            break;
        }
        uint8_t* planes[3] = {out + pl[r].off, out + pl[g].off, out + pl[b].off};
        int types[3] = {h->ch[r].type, h->ch[g].type, h->ch[b].type};
        bool lin[3] = {true, true, true};
        fine = dwa_lossy(3, planes, types, pl[r].nx, pl[r].ny, &acp, ace, &dcp, dce, lin,
                         D->dc_only ? (D->dc_sparse ? 2 : 1) : 0);
        done_ch[r] = done_ch[g] = done_ch[b] = true;
    }
    for (int c = 0; c < h->nch && fine; c++) {
        if (scheme[c] != DWA_LOSSY || done_ch[c]) continue;
        uint8_t* planes[3] = {out + pl[c].off, NULL, NULL};
        int types[3] = {h->ch[c].type, 0, 0};
        bool lin[3] = {!h->ch[c].plin, false, false};
        fine = dwa_lossy(1, planes, types, pl[c].nx, pl[c].ny, &acp, ace, &dcp, dce, lin,
                         D->dc_only ? (D->dc_sparse ? 2 : 1) : 0);
    }
    free(done_ch);
    if (!fine) goto done;

    // UNKNOWN channels: plain planes; RLE channels: one plane per byte
    {
        size_t uo = 0, ro = 0;
        for (int c = 0; c < h->nch; c++) {
            int ts = tsize(h->ch[c].type);
            size_t cntc = (size_t)pl[c].nx * pl[c].ny;
            if (scheme[c] == DWA_UNKNOWN) {
                memcpy(out + pl[c].off, unk_plane + uo, cntc * ts);
                uo += cntc * ts;
            } else if (scheme[c] == DWA_RLE) {
                uint8_t* o = out + pl[c].off;
                for (size_t i = 0; i < cntc; i++)
                    for (int bb = 0; bb < ts; bb++)
                        o[i * ts + bb] = rle_plane[ro + (size_t)bb * cntc + i];
                ro += cntc * ts;
            }
        }
    }
    ok = true;
done:
    free(rules);
    free(sets);
    free(scheme);
    return ok;
}

// ------------------------------------------------------------ one chunk
static int lines_per_chunk(int comp) {
    switch (comp) {
    case C_ZIP: case C_PXR24: return 16;
    case C_PIZ: case C_B44: case C_B44A: case C_DWAA: return 32;
    case C_DWAB: return 256;
    default: return 1;
    }
}

// Decode one chunk covering [x0..x1] x [y0..y1] into planar planes.
static bool decode_chunk(const XHdr* h, XDec* D, const uint8_t* data, size_t n,
                         int x0, int x1, int y0, int y1, XPlane* pl, uint8_t** outp) {
    size_t raw = 0;
    for (int c = 0; c < h->nch; c++) {
        pl[c].nx = num_samples(h->ch[c].xs, x0, x1);
        pl[c].ny = num_samples(h->ch[c].ys, y0, y1);
        pl[c].off = raw;
        raw += (size_t)pl[c].nx * pl[c].ny * tsize(h->ch[c].type);
        if (raw > EXR_MAX_CHUNK) return false;
    }
    uint8_t* out = (uint8_t*)calloc(1, raw + 16);
    if (!out) return false;
    *outp = out;
    if (h->comp == C_NONE || n >= raw) {
        if (n != raw) return false;
        return lines_to_planar(h, D, pl, y0, y1, data, n, out);
    }
    switch (h->comp) {
    case C_RLE: case C_ZIPS: case C_ZIP: {
        uint8_t* t = grow(&D->tmp, &D->tmp_cap, raw);
        uint8_t* l = grow(&D->tmp2, &D->tmp2_cap, raw);
        if (!t || !l) return false;
        long long got = h->comp == C_RLE ? rle_unc(data, n, t, raw) : zinflate(data, n, t, raw);
        if (got != (long long)raw) return false;
        unpredict_interleave(t, raw, l);
        return lines_to_planar(h, D, pl, y0, y1, l, raw, out);
    }
    case C_PIZ:   return piz_decode(h, D, data, n, pl, out, raw);
    case C_PXR24: return pxr24_decode(h, D, data, n, pl, y0, y1, out, raw);
    case C_B44: case C_B44A: return b44_decode(h, data, n, pl, out);
    case C_DWAA: case C_DWAB: return dwa_decode(h, D, data, n, pl, out);
    default: return false;
    }
}

// ------------------------------------------------------------ header parse
static const uint8_t* rd_cstr(const uint8_t* p, const uint8_t* end, char* dst, size_t cap) {
    size_t i = 0;
    while (p < end && *p) {
        if (i + 1 >= cap) return NULL;
        dst[i++] = (char)*p++;
    }
    if (p >= end) return NULL;
    dst[i] = 0;
    return p + 1;
}

static bool parse_chlist(const uint8_t* v, size_t sz, XHdr* h) {
    const uint8_t* q = v;
    const uint8_t* e = v + sz;
    h->nch = 0;
    while (q < e && *q) {
        if (h->nch >= EXR_MAX_CH) return false;
        XChan* c = &h->ch[h->nch];
        q = rd_cstr(q, e, c->name, sizeof c->name);
        if (!q || e - q < 16) return false;
        c->type = (int)rd32(q);
        c->plin = q[4];
        c->xs = (int)rd32(q + 8);
        c->ys = (int)rd32(q + 12);
        q += 16;
        if (c->type < 0 || c->type > 2 || c->xs < 1 || c->ys < 1 ||
            c->xs > EXR_MAX_DIM || c->ys > EXR_MAX_DIM)
            return false;
        h->nch++;
    }
    return h->nch > 0;
}

// keep == false: walk past a header without storing it (later parts).
static const uint8_t* parse_header(const uint8_t* p, const uint8_t* end, XHdr* h, bool keep) {
    char an[256], at[256];
    for (;;) {
        if (p >= end) return NULL;
        if (*p == 0) return p + 1;
        p = rd_cstr(p, end, an, sizeof an);
        if (!p) return NULL;
        p = rd_cstr(p, end, at, sizeof at);
        if (!p || end - p < 4) return NULL;
        uint32_t sz = rd32(p);
        p += 4;
        if (sz > (size_t)(end - p)) return NULL;
        const uint8_t* v = p;
        p += sz;
        if (!keep) continue;
        if (!strcmp(an, "channels") && !strcmp(at, "chlist")) {
            if (!parse_chlist(v, sz, h)) return NULL;
            h->has_ch = true;
        } else if (!strcmp(an, "compression") && sz >= 1) {
            h->comp = v[0];
        } else if (!strcmp(an, "dataWindow") && sz >= 16) {
            for (int i = 0; i < 4; i++) h->dw[i] = (int)rd32(v + 4 * i);
            h->has_dw = true;
        } else if (!strcmp(an, "displayWindow") && sz >= 16) {
            for (int i = 0; i < 4; i++) h->disp[i] = (int)rd32(v + 4 * i);
            h->has_disp = true;
        } else if (!strcmp(an, "pixelAspectRatio") && sz >= 4) {
            memcpy(&h->par, v, 4);
        } else if (!strcmp(an, "tiles") && sz >= 9) {
            h->tw = (int)rd32(v);
            h->th = (int)rd32(v + 4);
            h->tmode = v[8];
            h->has_tiles = true;
        } else if (!strcmp(an, "type") && !strcmp(at, "string")) {
            size_t k = sz < sizeof h->type - 1 ? sz : sizeof h->type - 1;
            memcpy(h->type, v, k);
            h->type[k] = 0;
        } else if (!strcmp(an, "chunkCount") && sz >= 4) {
            h->chunk_count = (int)rd32(v);
            h->has_cc = true;
        }
    }
}

// ------------------------------------------------------------ preview
typedef struct {
    int sel[4];          // R G B A channel index or -1
    bool gray;           // sel[0] is a luminance / single channel
    int dx0, dy0;        // data window origin
    int ow, oh;
    int* xmap;           // data-window column -> preview column (-1 outside)
    int* ymap;           // data-window row    -> preview row    (-1 outside)
    float* acc;          // ow * oh * 4 linear sums
} XAcc;

static float fetch(const uint8_t* row, int i, int type) {
    float v;
    if (type == PT_HALF) {
        v = g_h2f[rd16(row + 2 * (size_t)i)];
    } else if (type == PT_FLOAT) {
        memcpy(&v, row + 4 * (size_t)i, 4);
    } else {
        v = (float)rd32(row + 4 * (size_t)i);
    }
    if (!(v == v)) v = 0.0f;                      // NaN
    if (v > 65504.0f) v = 65504.0f;
    if (v < -65504.0f) v = -65504.0f;
    return v;
}

static void accumulate(const XHdr* h, XAcc* A, const XPlane* pl, const uint8_t* out,
                       int x0, int x1, int y0, int y1) {
    for (int y = y0; y <= y1; y++) {
        int oy = A->ymap[y - A->dy0];
        if (oy < 0) continue;
        const uint8_t* rows[4] = {NULL, NULL, NULL, NULL};
        int types[4] = {0, 0, 0, 0}, xs[4] = {1, 1, 1, 1};
        for (int k = 0; k < 4; k++) {
            int c = A->sel[k];
            if (c < 0) continue;
            int ys = h->ch[c].ys;
            int ry = ys == 1 ? y - y0 : num_samples(ys, y0, y) - 1;
            if (ry < 0) ry = 0;
            if (ry >= pl[c].ny) continue;
            rows[k] = out + pl[c].off + (size_t)ry * pl[c].nx * tsize(h->ch[c].type);
            types[k] = h->ch[c].type;
            xs[k] = h->ch[c].xs;
        }
        float* arow = A->acc + (size_t)oy * A->ow * 4;
        // fast path - the usual case: every used channel is HALF, unsampled
        bool fast = true;
        for (int k = 0; k < 4; k++) {
            int c = A->sel[k];
            if (c >= 0 && (!rows[k] || types[k] != PT_HALF || xs[k] != 1)) fast = false;
        }
        if (fast) {
            const uint16_t* r16[4];
            float cst[4];
            for (int k = 0; k < 4; k++) {
                r16[k] = (const uint16_t*)rows[k];
                cst[k] = (k == 3) ? 1.0f : 0.0f;
            }
            for (int x = x0; x <= x1; x++) {
                int ox = A->xmap[x - A->dx0];
                if (ox < 0) continue;
                float* a = arow + (size_t)ox * 4;
                int i = x - x0;
                for (int k = 0; k < 4; k++) {
                    uint16_t hv;
                    if (r16[k]) {
                        memcpy(&hv, r16[k] + i, 2);
                        a[k] += g_h2f_safe[hv];
                    } else {
                        a[k] += cst[k];
                    }
                }
            }
            continue;
        }
        for (int x = x0; x <= x1; x++) {
            int ox = A->xmap[x - A->dx0];
            if (ox < 0) continue;
            float* a = arow + (size_t)ox * 4;
            for (int k = 0; k < 4; k++) {
                float v;
                if (rows[k]) {
                    int rx = xs[k] == 1 ? x - x0 : num_samples(xs[k], x0, x) - 1;
                    if (rx < 0) rx = 0;
                    if (rx >= pl[A->sel[k]].nx) rx = pl[A->sel[k]].nx - 1;
                    v = fetch(rows[k], rx, types[k]);
                } else {
                    v = (k == 3) ? 1.0f : 0.0f;   // no alpha = opaque
                }
                a[k] += v;
            }
        }
    }
}

// Up to two (output index, pixel count) runs for one 8-pixel block span.
// A preview pixel covers >= 6 source pixels in DC mode, so a block can
// straddle at most two of them; -1 entries (outside the display window)
// carry no weight.
static int block_runs(const int* map, int start, int n, int idx[2], int cnt[2]) {
    int r = 0;
    for (int i = 0; i < n; i++) {
        int o = map[start + i];
        if (o < 0) continue;
        if (r > 0 && idx[r - 1] == o) { cnt[r - 1]++; continue; }
        if (r == 2) { cnt[1]++; continue; }      // cannot happen; stay safe
        idx[r] = o;
        cnt[r] = 1;
        r++;
    }
    return r;
}

// DC-only twin of accumulate(): every 8x8 block holds ONE value (written to
// its top-left pixel by dwa_lossy mode 2), so it is read once and added
// with the block's pixel count as weight - 1/64 of the reads, the same sums.
// Only used when every selected channel is unsampled (xs = ys = 1).
static void accumulate_dc(const XHdr* h, XAcc* A, const XPlane* pl, const uint8_t* out,
                          int x0, int x1, int y0, int y1) {
    for (int y = y0; y <= y1; y += 8) {
        int bh = (y1 - y + 1) < 8 ? (y1 - y + 1) : 8;
        int oyi[2], oyc[2];
        int ny = block_runs(A->ymap, y - A->dy0, bh, oyi, oyc);
        if (ny == 0) continue;
        const uint8_t* rows[4] = {NULL, NULL, NULL, NULL};
        int types[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
            int c = A->sel[k];
            if (c < 0 || y - y0 >= pl[c].ny) continue;
            rows[k] = out + pl[c].off + (size_t)(y - y0) * pl[c].nx * tsize(h->ch[c].type);
            types[k] = h->ch[c].type;
        }
        for (int x = x0; x <= x1; x += 8) {
            int bw = (x1 - x + 1) < 8 ? (x1 - x + 1) : 8;
            int oxi[2], oxc[2];
            int nx = block_runs(A->xmap, x - A->dx0, bw, oxi, oxc);
            if (nx == 0) continue;
            float v[4];
            for (int k = 0; k < 4; k++) {
                if (rows[k] && x - x0 < pl[A->sel[k]].nx) {
                    if (types[k] == PT_HALF)
                        v[k] = g_h2f_safe[rd16(rows[k] + 2 * (size_t)(x - x0))];
                    else
                        v[k] = fetch(rows[k], x - x0, types[k]);
                } else {
                    v[k] = (k == 3) ? 1.0f : 0.0f;    // no alpha = opaque
                }
            }
            for (int a = 0; a < ny; a++) {
                float* arow = A->acc + (size_t)oyi[a] * A->ow * 4;
                for (int b = 0; b < nx; b++) {
                    float wgt = (float)(oyc[a] * oxc[b]);
                    float* acc = arow + (size_t)oxi[b] * 4;
                    acc[0] += v[0] * wgt;
                    acc[1] += v[1] * wgt;
                    acc[2] += v[2] * wgt;
                    acc[3] += v[3] * wgt;
                }
            }
        }
    }
}

static int find_ch(const XHdr* h, const char* prefix, const char* const* names) {
    size_t pn = strlen(prefix);
    for (int i = 0; names[i]; i++) {
        for (int c = 0; c < h->nch; c++) {
            const char* nm = h->ch[c].name;
            if (pn) {
                if (strncmp(nm, prefix, pn) || nm[pn] != '.') continue;
                nm += pn + 1;
            } else if (strchr(nm, '.')) {
                continue;
            }
            if (str_ieq(nm, names[i])) return c;
        }
    }
    return -1;
}

static void pick_channels(const XHdr* h, XAcc* A) {
    static const char* const R[] = {"R", "red", NULL};
    static const char* const G[] = {"G", "green", NULL};
    static const char* const B[] = {"B", "blue", NULL};
    static const char* const AL[] = {"A", "alpha", NULL};
    static const char* const Y[] = {"Y", "L", "luminance", NULL};
    A->gray = false;
    char prefix[256] = "";
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {          // no top-level RGB: the first layer that has it
            prefix[0] = 0;
            for (int c = 0; c < h->nch; c++) {
                const char* dot = strrchr(h->ch[c].name, '.');
                if (dot && (str_ieq(dot + 1, "R") || str_ieq(dot + 1, "red"))) {
                    size_t pn = (size_t)(dot - h->ch[c].name);
                    memcpy(prefix, h->ch[c].name, pn);
                    prefix[pn] = 0;
                    break;
                }
            }
            if (!prefix[0]) break;
        }
        A->sel[0] = find_ch(h, prefix, R);
        A->sel[1] = find_ch(h, prefix, G);
        A->sel[2] = find_ch(h, prefix, B);
        A->sel[3] = find_ch(h, prefix, AL);
        if (A->sel[0] >= 0 || A->sel[1] >= 0 || A->sel[2] >= 0) return;
    }
    A->sel[0] = find_ch(h, "", Y);
    A->sel[1] = A->sel[2] = -1;
    A->sel[3] = find_ch(h, "", AL);
    A->gray = true;
    if (A->sel[0] >= 0) return;
    A->sel[0] = 0;                // anything at all: the first channel as grey
    A->sel[3] = -1;
}

// ------------------------------------------------------------ chunk runner
// Chunks are independent, so one frame decodes on several cores at once.
// Each thread owns its decoder scratch (XDec), plane table and accumulator;
// the maps, header and file bytes are shared read-only.  Thread k takes
// chunks k, k+T, k+2T... (interleaved: equal work for every thread even
// when one end of the frame is empty).  The partial sums are added at the
// end, so the result is the same as a one-thread run (up to float order).
typedef struct {
    const XHdr* h;
    const uint8_t* data;
    size_t size;
    const uint8_t* end;
    const uint8_t* table;
    bool multipart, tiled;
    int ncx, cw, chh;
    long long nchunks;
    int dx0, dy0, dx1, dy1;
    XAcc* A;                 // shared maps; A->acc is thread 0's sum buffer
    bool dc_only, dc_sparse;
} XRun;

typedef struct {
    const XRun* R;
    int tid, nt;
    float* acc;              // this thread's sums (ow * oh * 4)
    int decoded;
    bool ok;                 // its scratch allocated (else it did nothing)
} XWork;

static void run_slice(XWork* w) {
    const XRun* R = w->R;
    const XHdr* h = R->h;
    XAcc A = *R->A;
    A.acc = w->acc;
    XDec D;
    memset(&D, 0, sizeof D);
    D.dc_only = R->dc_only;
    D.dc_sparse = R->dc_sparse;
    XPlane* pl = (XPlane*)malloc((size_t)h->nch * sizeof(XPlane));
    D.rowc = (int*)malloc((size_t)h->nch * sizeof(int));
    w->ok = pl && D.rowc;
    for (long long ci = w->tid; w->ok && ci < R->nchunks; ci += w->nt) {
        int tx = (int)(ci % R->ncx), ty = (int)(ci / R->ncx);
        int x0 = R->dx0 + tx * R->cw, y0 = R->dy0 + ty * R->chh;
        int x1 = x0 + R->cw - 1, y1 = y0 + R->chh - 1;
        if (x1 > R->dx1) x1 = R->dx1;
        if (y1 > R->dy1) y1 = R->dy1;
        // nothing of this chunk lands in the preview: skip the decode
        bool any = false;
        for (int y = y0; y <= y1 && !any; y++) any = A.ymap[y - R->dy0] >= 0;
        if (!any) continue;
        uint64_t off = rd64(R->table + 8 * ci);
        if (off < 8 || off >= R->size) continue;
        const uint8_t* c = R->data + off;
        size_t room = (size_t)(R->end - c);
        if (R->multipart) {
            if (room < 4 || rd32(c) != 0) continue;
            c += 4;
            room -= 4;
        }
        uint32_t dsz;
        if (R->tiled) {
            if (room < 20) continue;
            if ((int)rd32(c) != tx || (int)rd32(c + 4) != ty || rd32(c + 8) || rd32(c + 12))
                continue;
            dsz = rd32(c + 16);
            c += 20;
            room -= 20;
        } else {
            if (room < 8 || (int)rd32(c) != y0) continue;
            dsz = rd32(c + 4);
            c += 8;
            room -= 8;
        }
        if (dsz > room) continue;
        uint8_t* planes = NULL;
        bool good = decode_chunk(h, &D, c, dsz, x0, x1, y0, y1, pl, &planes);
        if (good) {
            if (D.dc_sparse) accumulate_dc(h, &A, pl, planes, x0, x1, y0, y1);
            else accumulate(h, &A, pl, planes, x0, x1, y0, y1);
            w->decoded++;
        }
        free(planes);
    }
    free(pl);
    free(D.rowc);
    free(D.huf);
    free(D.lut);
    free(D.tmp);
    free(D.tmp2);
    free(D.tmp3);
}

static DWORD WINAPI run_slice_thread(LPVOID p) {
    run_slice((XWork*)p);
    return 0;
}

static int exr_cpu_count(void) {
    static volatile LONG n = 0;
    if (n == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        InterlockedExchange(&n, si.dwNumberOfProcessors > 0 ? (LONG)si.dwNumberOfProcessors : 1);
    }
    return (int)n;
}

#define EXR_MAX_THREADS 8

// Decode every chunk that reaches the preview; returns how many decoded.
static int exr_run_chunks(const XRun* R, long long W, long long H) {
    XAcc* A = R->A;
    size_t accn = (size_t)A->ow * A->oh * 4;
    // threads: one per ~1 Mpx of real decode work, at most the cores
    // (one left for the UI) and EXR_MAX_THREADS; extra accumulators are
    // capped at ~64 MB so a huge preview never balloons memory
    double work = (double)W * (double)H / (R->dc_only ? 64.0 : 1.0);
    int nt = (int)(work / (1024.0 * 1024.0)) + 1;
    int cpus = exr_cpu_count() - 1;
    if (cpus < 1) cpus = 1;
    if (nt > cpus) nt = cpus;
    if (nt > EXR_MAX_THREADS) nt = EXR_MAX_THREADS;
    if (nt > R->nchunks) nt = (int)R->nchunks;
    size_t accb = accn * sizeof(float);
    if (accb > 0 && (size_t)(nt - 1) * accb > (size_t)64 * 1024 * 1024)
        nt = (int)((size_t)64 * 1024 * 1024 / accb) + 1;
    if (nt < 1) nt = 1;

    XWork wk[EXR_MAX_THREADS];
    HANDLE th[EXR_MAX_THREADS];
    memset(wk, 0, sizeof wk);
    memset(th, 0, sizeof th);
    int prio = GetThreadPriority(GetCurrentThread());
    for (int t = 0; t < nt; t++) {
        wk[t].R = R;
        wk[t].tid = t;
        wk[t].nt = nt;
        wk[t].acc = (t == 0) ? A->acc : (float*)calloc(accn, sizeof(float));
    }
    // a helper that cannot get its buffer or thread: its slice runs here
    for (int t = 1; t < nt; t++) {
        if (!wk[t].acc) continue;
        th[t] = CreateThread(NULL, 0, run_slice_thread, &wk[t], CREATE_SUSPENDED, NULL);
        if (th[t]) {
            if (prio != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(th[t], prio);
            ResumeThread(th[t]);
        }
    }
    run_slice(&wk[0]);
    for (int t = 1; t < nt; t++) {
        if (th[t]) {
            WaitForSingleObject(th[t], INFINITE);
            CloseHandle(th[t]);
        } else {
            float* own = wk[t].acc;
            wk[t].acc = own ? own : A->acc;   // no buffer: sum straight in
            run_slice(&wk[t]);
            if (!own) wk[t].acc = NULL;
        }
    }
    int decoded = 0;
    for (int t = 0; t < nt; t++) {
        decoded += wk[t].decoded;
        if (t > 0 && wk[t].acc) {
            for (size_t i = 0; i < accn; i++) A->acc[i] += wk[t].acc[i];
            free(wk[t].acc);
        }
    }
    return decoded;
}

static bool exr_preview_impl(const uint8_t* data, size_t size, int max_w, int max_h,
                             unsigned bg, uint8_t** out_bgra, int* out_w, int* out_h) {
    if (size < 8 || rd32(data) != 20000630u) return false;
    uint32_t ver = rd32(data + 4);
    if ((ver & 0xff) != 2) return false;
    bool multipart = (ver & 0x1000) != 0;
    if (ver & 0x800) return false;                     // deep data
    exr_tables();

    XHdr* h = (XHdr*)calloc(1, sizeof(XHdr));
    if (!h) return false;
    h->par = 1.0f;
    const uint8_t* end = data + size;
    const uint8_t* p = parse_header(data + 8, end, h, true);
    if (p && multipart) {
        XHdr* skip = (XHdr*)calloc(1, sizeof(XHdr));
        while (p && skip && p < end && *p) p = parse_header(p, end, skip, false);
        free(skip);
        if (p && p < end) p++;                         // end of the header list
        else p = NULL;
    }
    bool ok = false;
    XAcc A;
    memset(&A, 0, sizeof A);
    XDec D;
    memset(&D, 0, sizeof D);
    XPlane* pl = NULL;
    if (!p || !h->has_ch || !h->has_dw) goto fail;
    if (strstr(h->type, "deep")) goto fail;
    bool tiled = multipart ? (strstr(h->type, "tiled") != NULL) : (ver & 0x200) != 0;
    if (h->comp < 0 || h->comp > C_DWAB) goto fail;
    int dx0 = h->dw[0], dy0 = h->dw[1], dx1 = h->dw[2], dy1 = h->dw[3];
    if (dx1 < dx0 || dy1 < dy0) goto fail;
    long long W = (long long)dx1 - dx0 + 1, H = (long long)dy1 - dy0 + 1;
    if (W > EXR_MAX_DIM || H > EXR_MAX_DIM) goto fail;
    if (tiled && (!h->has_tiles || h->tw < 1 || h->th < 1 ||
                  h->tw > EXR_MAX_DIM || h->th > EXR_MAX_DIM))
        goto fail;
    if (!(h->par > 0.01f && h->par < 100.0f)) h->par = 1.0f;

    // framing: the display window (clipped data outside it is cut off,
    // missing data inside it is transparent) - what every EXR viewer shows
    int fx0 = dx0, fy0 = dy0, fx1 = dx1, fy1 = dy1;
    if (h->has_disp && h->disp[2] >= h->disp[0] && h->disp[3] >= h->disp[1] &&
        h->disp[0] <= dx1 && h->disp[2] >= dx0 && h->disp[1] <= dy1 && h->disp[3] >= dy0 &&
        (long long)h->disp[2] - h->disp[0] < EXR_MAX_DIM &&
        (long long)h->disp[3] - h->disp[1] < EXR_MAX_DIM) {
        fx0 = h->disp[0]; fy0 = h->disp[1]; fx1 = h->disp[2]; fy1 = h->disp[3];
    }
    int fw = fx1 - fx0 + 1, fh = fy1 - fy0 + 1;
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;
    double dispw = fw * (double)h->par, disph = fh;
    double s = (double)max_w / dispw;
    if ((double)max_h / disph < s) s = (double)max_h / disph;
    if (s * dispw > fw) s = fw / dispw;            // never larger than the
    if (s * disph > fh) s = fh / disph;            // pixels that exist
    int ow = (int)(dispw * s + 0.5), oh = (int)(disph * s + 0.5);
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    if (ow > fw) ow = fw;
    if (oh > fh) oh = fh;
    // DWA frame 6x+ bigger than the preview: build it from the 8x8 block
    // averages alone (no AC data, no inverse DCT - ~10x faster) and cap the
    // preview at that 1/8 resolution. The cap costs at most 25% size versus
    // the request, invisible in a cell that size.
    bool dwa = (h->comp == C_DWAA || h->comp == C_DWAB);
    if (dwa && fw >= ow * 6 && fh >= oh * 6 && (ow * 8 > fw || oh * 8 > fh)) {
        double s8 = s;
        if (dispw * s8 > fw / 8.0) s8 = (fw / 8.0) / dispw;
        if (disph * s8 > fh / 8.0) s8 = (fh / 8.0) / disph;
        ow = (int)(dispw * s8);
        oh = (int)(disph * s8);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
    }

    A.dx0 = dx0;
    A.dy0 = dy0;
    A.ow = ow;
    A.oh = oh;
    A.xmap = (int*)malloc((size_t)W * sizeof(int));
    A.ymap = (int*)malloc((size_t)H * sizeof(int));
    A.acc = (float*)calloc((size_t)ow * oh * 4, sizeof(float));
    if (!A.xmap || !A.ymap || !A.acc) goto fail;
    for (long long x = 0; x < W; x++) {
        long long fx = dx0 + x - fx0;
        A.xmap[x] = (fx < 0 || fx >= fw) ? -1 : (int)(fx * ow / fw);
    }
    for (long long y = 0; y < H; y++) {
        long long fy = dy0 + y - fy0;
        A.ymap[y] = (fy < 0 || fy >= fh) ? -1 : (int)(fy * oh / fh);
    }
    pick_channels(h, &A);
    D.dc_only = (h->comp == C_DWAA || h->comp == C_DWAB) && fw >= ow * 8 && fh >= oh * 8;
    D.dc_sparse = D.dc_only;
    for (int k = 0; k < 4; k++)
        if (A.sel[k] >= 0 && (h->ch[A.sel[k]].xs != 1 || h->ch[A.sel[k]].ys != 1))
            D.dc_sparse = false;

    // chunk list
    int ncx = 1, ncy;
    int cw = (int)W, chh;
    if (tiled) {
        cw = h->tw;
        chh = h->th;
        ncx = (int)((W + cw - 1) / cw);
        ncy = (int)((H + chh - 1) / chh);
    } else {
        chh = lines_per_chunk(h->comp);
        ncy = (int)((H + chh - 1) / chh);
    }
    long long nchunks = (long long)ncx * ncy;
    if (nchunks > 64 * 1024 * 1024 || (size_t)(end - p) / 8 < (size_t)nchunks) goto fail;
    const uint8_t* table = p;
    XRun run;
    memset(&run, 0, sizeof run);
    run.h = h; run.data = data; run.size = size; run.end = end; run.table = table;
    run.multipart = multipart; run.tiled = tiled;
    run.ncx = ncx; run.cw = cw; run.chh = chh; run.nchunks = nchunks;
    run.dx0 = dx0; run.dy0 = dy0; run.dx1 = dx1; run.dy1 = dy1;
    run.A = &A;
    run.dc_only = D.dc_only; run.dc_sparse = D.dc_sparse;
    int decoded = exr_run_chunks(&run, W, H);
    if (!decoded) goto fail;

    // average, composite over the cell colour, encode sRGB
    {
        int* cc = (int*)calloc((size_t)ow, sizeof(int));
        int* rc = (int*)calloc((size_t)oh, sizeof(int));
        uint8_t* img = (uint8_t*)malloc((size_t)ow * oh * 4);
        if (!cc || !rc || !img) { free(cc); free(rc); free(img); goto fail; }
        for (int x = 0; x < fw; x++) cc[(long long)x * ow / fw]++;
        for (int y = 0; y < fh; y++) rc[(long long)y * oh / fh]++;
        float bgl[3];
        for (int k = 0; k < 3; k++) {
            double v = ((bg >> (16 - 8 * k)) & 0xff) / 255.0;
            bgl[k] = (float)(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
        }
        for (int oy = 0; oy < oh; oy++) {
            for (int ox = 0; ox < ow; ox++) {
                const float* a = A.acc + ((size_t)oy * ow + ox) * 4;
                float area = (float)cc[ox] * (float)rc[oy];
                float inv = area > 0 ? 1.0f / area : 0.0f;
                float r = a[0] * inv, g = a[1] * inv, b = a[2] * inv, al = a[3] * inv;
                if (A.gray) g = b = r;
                if (al < 0) al = 0;
                if (al > 1) al = 1;
                float lin[3] = {r + bgl[0] * (1 - al), g + bgl[1] * (1 - al), b + bgl[2] * (1 - al)};
                uint8_t* o = img + ((size_t)oy * ow + ox) * 4;
                for (int k = 0; k < 3; k++) {
                    float v = lin[k];
                    if (!(v == v)) v = 0.0f;
                    int idx = v <= 0 ? 0 : v >= 1 ? 4096 : (int)(v * 4096.0f + 0.5f);
                    o[2 - k] = g_srgb[idx];          // BGRA
                }
                o[3] = 255;
            }
        }
        free(cc);
        free(rc);
        *out_bgra = img;
        *out_w = ow;
        *out_h = oh;
        ok = true;
    }
fail:
    free(A.xmap);
    free(A.ymap);
    free(A.acc);
    free(pl);
    free(D.rowc);
    free(D.huf);
    free(D.lut);
    free(D.tmp);
    free(D.tmp2);
    free(D.tmp3);
    free(h);
    return ok;
}

bool exr_preview_mem(const unsigned char* data, size_t size, int max_w, int max_h,
                     unsigned bg, unsigned char** out_bgra, int* out_w, int* out_h) {
    if (!data || !out_bgra || !out_w || !out_h) return false;
    *out_bgra = NULL;
    return exr_preview_impl(data, size, max_w, max_h, bg, out_bgra, out_w, out_h);
}

bool exr_preview_file(const char* path, int max_w, int max_h, unsigned bg,
                      unsigned char** out_bgra, int* out_w, int* out_h) {
    if (!path || !path[0] || !out_bgra) return false;
    *out_bgra = NULL;
    HANDLE f = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = false;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart >= 8 && (unsigned long long)sz.QuadPart < ((size_t)-1) / 2) {
        HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (m) {
            const unsigned char* v = (const unsigned char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            if (v) {
                ok = exr_preview_mem(v, (size_t)sz.QuadPart, max_w, max_h, bg, out_bgra,
                                     out_w, out_h);
                UnmapViewOfFile(v);
            }
            CloseHandle(m);
        }
    }
    CloseHandle(f);
    return ok;
}
