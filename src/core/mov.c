// mov.c - ProRes .mov thumbnails (see include/mov.h).
//
// Two parts:
//  1. A minimal QuickTime / ISO-BMFF reader: moov > trak > mdia >
//     (hdlr "vide", minf > stbl > stsd / stsz / stsc / stco|co64) gives
//     the codec and the file offset + size of the chosen frame.
//  2. A DC-only ProRes decoder (SMPTE RDD 36): every 8x8 block's DC
//     coefficient is entropy-coded first in each slice, so a thumbnail
//     reads just those and never touches the AC data or an inverse DCT.
//     The DC of a block is its average, so the result is the frame box-
//     filtered by 8 - exactly what a thumbnail needs.
//
// Everything is bounds-checked: a damaged or hostile file returns false,
// it never reads outside the buffer it was given.
#include "mov.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// ------------------------------------------------------------ helpers
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

bool mov_is_mov_name(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    if (n < 4) return false;
    const char* e = name + n - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'm' && (e[2] | 0x20) == 'o' && (e[3] | 0x20) == 'v';
}

// ------------------------------------------------------------ container
typedef struct {
    const uint8_t* p;     // payload
    uint64_t n;           // payload size
} Box;

// Find the first child box of `type` inside [p, p+n).
static bool find_box(const uint8_t* p, uint64_t n, const char* type, Box* out) {
    uint64_t off = 0;
    while (n - off >= 8) {
        uint64_t sz = be32(p + off);
        uint64_t hdr = 8;
        if (sz == 1) {
            if (n - off < 16) return false;
            sz = be64(p + off + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = n - off;                 // runs to the end of its parent
        }
        if (sz < hdr || sz > n - off) return false;
        if (memcmp(p + off + 4, type, 4) == 0) {
            out->p = p + off + hdr;
            out->n = sz - hdr;
            return true;
        }
        off += sz;
    }
    return false;
}

static bool is_prores(const uint8_t* f) {
    static const char* const tags[] = {"apco", "apcs", "apcn", "apch", "ap4h", "ap4x", NULL};
    for (int i = 0; tags[i]; i++)
        if (memcmp(f, tags[i], 4) == 0) return true;
    return false;
}

// The video track's codec fourcc and the byte range of one frame (10% in,
// the way Explorer picks a frame, so a fade-in does not give a black tile).
static bool mov_find_frame(const uint8_t* d, size_t size, uint8_t fourcc[4],
                           uint64_t* foff, uint32_t* fsz) {
    Box moov;
    if (!find_box(d, size, "moov", &moov)) return false;
    uint64_t off = 0;
    while (moov.n - off >= 8) {               // walk every trak
        Box any;
        const uint8_t* q = moov.p + off;
        uint64_t sz = be32(q), hdr = 8;
        if (sz == 1) {
            if (moov.n - off < 16) return false;
            sz = be64(q + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = moov.n - off;
        }
        if (sz < hdr || sz > moov.n - off) return false;
        off += sz;
        if (memcmp(q + 4, "trak", 4) != 0) continue;
        any.p = q + hdr;
        any.n = sz - hdr;
        Box mdia, hdlr, minf, stbl, stsd, stsz, stsc, stco;
        if (!find_box(any.p, any.n, "mdia", &mdia)) continue;
        if (!find_box(mdia.p, mdia.n, "hdlr", &hdlr) || hdlr.n < 12 ||
            memcmp(hdlr.p + 8, "vide", 4) != 0)
            continue;
        if (!find_box(mdia.p, mdia.n, "minf", &minf) ||
            !find_box(minf.p, minf.n, "stbl", &stbl) ||
            !find_box(stbl.p, stbl.n, "stsd", &stsd) || stsd.n < 16 ||
            !find_box(stbl.p, stbl.n, "stsz", &stsz) || stsz.n < 12 ||
            !find_box(stbl.p, stbl.n, "stsc", &stsc) || stsc.n < 8)
            continue;
        bool c64 = false;
        if (!find_box(stbl.p, stbl.n, "stco", &stco)) {
            if (!find_box(stbl.p, stbl.n, "co64", &stco)) continue;
            c64 = true;
        }
        if (stco.n < 8) continue;
        memcpy(fourcc, stsd.p + 12, 4);       // first sample entry's format
        uint32_t fixed = be32(stsz.p + 4), count = be32(stsz.p + 8);
        if (count == 0) continue;
        if (fixed == 0 && (uint64_t)count * 4 > stsz.n - 12) continue;
        uint32_t want = count / 10;
        uint32_t nchunks = be32(stco.p + 4);
        uint64_t esz = c64 ? 8 : 4;
        if ((uint64_t)nchunks * esz > stco.n - 8) continue;
        uint32_t nsc = be32(stsc.p + 4);
        if ((uint64_t)nsc * 12 > stsc.n - 8 || nsc == 0) continue;
        // walk chunk by chunk to the chunk holding sample `want`
        uint32_t sample = 0;
        for (uint32_t e = 0; e < nsc; e++) {
            const uint8_t* se = stsc.p + 8 + 12 * (size_t)e;
            uint32_t first = be32(se), per = be32(se + 4);
            uint32_t last = (e + 1 < nsc) ? be32(se + 12) : nchunks + 1;  // exclusive
            if (first < 1 || last <= first || per == 0) break;
            for (uint32_t c = first; c < last && c <= nchunks; c++) {
                if (want < sample + per) {
                    const uint8_t* ce = stco.p + 8 + esz * (c - 1);
                    uint64_t pos = c64 ? be64(ce) : be32(ce);
                    for (uint32_t s = sample; s < want; s++)
                        pos += fixed ? fixed : be32(stsz.p + 12 + 4 * (size_t)s);
                    uint32_t n = fixed ? fixed : be32(stsz.p + 12 + 4 * (size_t)want);
                    if (pos > size || n > size - pos) return false;
                    *foff = pos;
                    *fsz = n;
                    return true;
                }
                sample += per;
            }
        }
        return false;
    }
    return false;
}

// ------------------------------------------------------------ bit reader
typedef struct {
    const uint8_t* p;
    size_t nbits;          // total bits available
    size_t pos;            // bit position
} Bits;

// The next 32 bits (zeros past the end), MSB first.
static uint32_t peek32(const Bits* b) {
    size_t byte = b->pos >> 3;
    size_t nbytes = (b->nbits + 7) >> 3;
    uint64_t w = 0;
    for (int i = 0; i < 5; i++)
        w = (w << 8) | ((byte + i < nbytes) ? b->p[byte + i] : 0);
    return (uint32_t)(w >> (8 - (b->pos & 7)));
}

static bool bits_left(const Bits* b) { return b->pos < b->nbits; }

// One ProRes codeword (adaptive Rice / exp-Golomb, RDD 36 7.1.1).
static bool read_codeword(Bits* b, int cb, unsigned* val) {
    int switch_bits = cb & 3, rice_order = cb >> 5, exp_order = (cb >> 2) & 7;
    uint32_t buf = peek32(b);
    if (buf == 0 || !bits_left(b)) return false;
    int q = 0;
    while (!(buf & 0x80000000u)) { buf <<= 1; q++; }
    if (q > switch_bits) {
        int bits = exp_order - switch_bits + (q << 1);
        if (bits > 32 || bits < 1) return false;
        uint32_t v = peek32(b) >> (32 - bits);
        *val = v - (1u << exp_order) + ((unsigned)(switch_bits + 1) << rice_order);
        b->pos += (size_t)bits;
    } else if (rice_order) {
        b->pos += (size_t)q + 1;
        uint32_t v = peek32(b) >> (32 - rice_order);
        *val = ((unsigned)q << rice_order) + v;
        b->pos += (size_t)rice_order;
    } else {
        *val = (unsigned)q;
        b->pos += (size_t)q + 1;
    }
    return b->pos <= b->nbits;
}

// All DC coefficients of one component of one slice (RDD 36 7.1.1.1),
// dequantised to the block's 10-bit average.
static bool decode_dc(const uint8_t* p, size_t n, int nblocks, int q0, uint16_t* out) {
    static const uint8_t dc_cb[7] = {0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70};
    Bits b = {p, n * 8, 0};
    unsigned code;
    if (!read_codeword(&b, 0xB8, &code)) return false;
    int prev = (int)((code >> 1) ^ (unsigned)(-(int)(code & 1)));
    int sign = 0;
    code = 5;              // the second DC always starts on codebook 5
    for (int i = 0; i < nblocks; i++) {
        if (i > 0) {
            if (!read_codeword(&b, dc_cb[code < 6 ? code : 6], &code)) return false;
            if (code) sign ^= -(int)(code & 1);
            else sign = 0;
            prev += (int)((((code + 1) >> 1) ^ (unsigned)sign) - (unsigned)sign);
        }
        // dequantised DC = 32 x (block mean - 512) in the 10-bit domain
        // (checked against a reference decoder on every profile: the
        // block means match to within 1 code value)
        int v = 512 + (prev * q0) / 32;
        out[i] = (uint16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v);
    }
    return true;
}

// ------------------------------------------------------------ ProRes DC
typedef struct {
    int gw, gh;            // DC grid (luma 8x8 blocks) of the frame
    uint16_t* y;           // gw * gh
    uint16_t* cb;          // gw * gh (chroma spread onto the luma grid)
    uint16_t* cr;
} DcImg;

static bool prores_dc(const uint8_t* f, size_t fn, DcImg* img, int* matrix) {
    if (fn < 8 + 20 || memcmp(f + 4, "icpf", 4) != 0) return false;
    uint32_t fsz = be32(f);
    if (fsz > fn) fsz = (uint32_t)fn;
    const uint8_t* h = f + 8;
    size_t hn = fsz - 8;
    int hdr = be16(h);
    if (hdr < 20 || (size_t)hdr > hn) return false;
    int width = be16(h + 8), height = be16(h + 10);
    if (width < 1 || height < 1 || width > 16384 || height > 16384) return false;
    int chroma = h[12] >> 6;                  // 2 = 4:2:2, 3 = 4:4:4
    int interlace = (h[12] >> 2) & 3;
    if (chroma != 2 && chroma != 3) return false;
    *matrix = h[16];
    int qflags = h[19];
    uint8_t lq0 = 4, cq0 = 4;                 // default matrices are all 4
    const uint8_t* qp = h + 20;
    if (qflags & 2) {
        if (hdr < 20 + 64) return false;
        lq0 = qp[0];
        qp += 64;
    }
    if (qflags & 1) {
        if ((size_t)(qp - h) + 64 > (size_t)hdr) return false;
        cq0 = qp[0];
    } else {
        cq0 = lq0;
    }
    int mbw = (width + 15) >> 4;
    int fields = interlace ? 2 : 1;
    int mbh = interlace ? (height + 31) >> 5 : (height + 15) >> 4;
    // the first picture only: for interlaced material that is one field,
    // whose rows stand for two frame rows each
    const uint8_t* pic = h + hdr;
    size_t pn = hn - (size_t)hdr;
    if (pn < 8) return false;
    int phdr = pic[0] >> 3;
    if (phdr < 8 || (size_t)phdr > pn) return false;
    uint32_t psize = be32(pic + 1);
    if (psize > pn || psize < (uint32_t)phdr) psize = (uint32_t)pn;
    int nslices = be16(pic + 5);
    int log2w = pic[7] >> 4;
    if (log2w > 3 || nslices < 1) return false;
    const uint8_t* idx = pic + phdr;
    if ((size_t)phdr + (size_t)nslices * 2 > psize) return false;
    const uint8_t* data = idx + (size_t)nslices * 2;
    const uint8_t* end = pic + psize;

    int gw = mbw * 2, gh_field = mbh * 2;
    img->gw = (width + 7) / 8;
    img->gh = (height + 7) / 8;
    size_t gn = (size_t)img->gw * img->gh;
    img->y = (uint16_t*)malloc(gn * 2);
    img->cb = (uint16_t*)malloc(gn * 2);
    img->cr = (uint16_t*)malloc(gn * 2);
    uint16_t* fy = (uint16_t*)calloc((size_t)gw * gh_field, 2);
    uint16_t* fcb = (uint16_t*)calloc((size_t)gw * gh_field, 2);
    uint16_t* fcr = (uint16_t*)calloc((size_t)gw * gh_field, 2);
    uint16_t tmp[3][8 * 4 * 2];               // up to 8 MBs x 4 blocks
    bool ok = img->y && img->cb && img->cr && fy && fcb && fcr;
    int mbx = 0, mby = 0, decoded = 0;
    for (int s = 0; ok && s < nslices && mby < mbh; s++) {
        int mbc = 1 << log2w;
        while (mbw - mbx < mbc) mbc >>= 1;
        int ssz = be16(idx + 2 * s);
        const uint8_t* sp = data;
        data += ssz;
        if (data > end || ssz < 6) { ok = false; break; }
        int shdr = sp[0] >> 3;
        int q = sp[1];
        if (q < 1) q = 1;
        if (q > 224) q = 224;
        if (q > 128) q = (q - 96) << 2;
        int ysz = be16(sp + 2), usz = be16(sp + 4);
        int vsz = ssz - shdr - ysz - usz;
        if (shdr > 7 && ssz >= 8) vsz = be16(sp + 6);
        if (shdr < 6 || ysz < 0 || usz < 0 || vsz < 0 || shdr + ysz + usz + vsz > ssz) {
            ok = false;
            break;
        }
        int cblk = chroma == 2 ? 2 : 4;       // chroma blocks per MB
        bool good = decode_dc(sp + shdr, (size_t)ysz, mbc * 4, lq0 * q, tmp[0]) &&
                    decode_dc(sp + shdr + ysz, (size_t)usz, mbc * cblk, cq0 * q, tmp[1]) &&
                    decode_dc(sp + shdr + ysz + usz, (size_t)vsz, mbc * cblk, cq0 * q, tmp[2]);
        if (good) {
            for (int m = 0; m < mbc; m++) {
                for (int k = 0; k < 4; k++) {   // luma: TL TR BL BR
                    int gx = (mbx + m) * 2 + (k & 1), gy = mby * 2 + (k >> 1);
                    size_t at = (size_t)gy * gw + gx;
                    fy[at] = tmp[0][m * 4 + k];
                    // chroma: 4:2:2 has a top and a bottom block per MB;
                    // 4:4:4 has four, in COLUMN order (TL BL TR BR) -
                    // unlike luma's row order
                    int ci = chroma == 2 ? m * 2 + (k >> 1)
                                         : m * 4 + ((k & 1) << 1) + (k >> 1);
                    fcb[at] = tmp[1][ci];
                    fcr[at] = tmp[2][ci];
                }
            }
            decoded++;
        }
        mbx += mbc;
        if (mbx >= mbw) { mbx = 0; mby++; }
    }
    if (ok && decoded > 0) {
        for (int y = 0; y < img->gh; y++) {
            int sy = (fields == 2) ? y / 2 : y;     // a field row = 2 frame rows
            if (sy >= gh_field) sy = gh_field - 1;
            for (int x = 0; x < img->gw; x++) {
                size_t a = (size_t)sy * gw + x, o = (size_t)y * img->gw + x;
                img->y[o] = fy[a];
                img->cb[o] = fcb[a];
                img->cr[o] = fcr[a];
            }
        }
    } else {
        ok = false;
    }
    free(fy);
    free(fcb);
    free(fcr);
    if (!ok) {
        free(img->y);
        free(img->cb);
        free(img->cr);
        img->y = img->cb = img->cr = NULL;
    }
    return ok;
}

// ------------------------------------------------------------ preview
bool mov_preview_mem(const unsigned char* data, size_t size, int max_w, int max_h,
                     unsigned char** out_bgra, int* out_w, int* out_h) {
    if (!data || !out_bgra || !out_w || !out_h || size < 16) return false;
    *out_bgra = NULL;
    uint8_t fourcc[4];
    uint64_t foff = 0;
    uint32_t fsz = 0;
    if (!mov_find_frame(data, size, fourcc, &foff, &fsz)) return false;
    if (!is_prores(fourcc)) return false;
    DcImg img;
    memset(&img, 0, sizeof img);
    int matrix = 0;
    if (!prores_dc(data + foff, fsz, &img, &matrix)) return false;

    // BT.709 for HD, 601 for SD, 2020 when tagged so (limited range)
    double kr = 0.2126, kb = 0.0722;
    if (matrix == 5 || matrix == 6 || (matrix != 1 && matrix != 9 && img.gh * 8 <= 576)) {
        kr = 0.299; kb = 0.114;
    } else if (matrix == 9) {
        kr = 0.2627; kb = 0.0593;
    }
    double kg = 1.0 - kr - kb;
    float rcr = (float)(2 * (1 - kr)), bcb = (float)(2 * (1 - kb));
    float gcb = (float)(2 * kb * (1 - kb) / kg), gcr = (float)(2 * kr * (1 - kr) / kg);

    // fit, never bigger than the DC grid
    int ow = img.gw, oh = img.gh;
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;
    double s = (double)max_w / ow;
    if ((double)max_h / oh < s) s = (double)max_h / oh;
    if (s < 1.0) {
        ow = (int)(img.gw * s + 0.5);
        oh = (int)(img.gh * s + 0.5);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
    }
    uint8_t* rgb = (uint8_t*)malloc((size_t)img.gw * img.gh * 3);
    uint8_t* out = (uint8_t*)malloc((size_t)ow * oh * 4);
    bool ok = rgb && out;
    if (ok) {
        for (size_t i = 0; i < (size_t)img.gw * img.gh; i++) {
            float y = (img.y[i] - 64) / 876.0f;
            float cb = (img.cb[i] - 512) / 896.0f;
            float cr = (img.cr[i] - 512) / 896.0f;
            float c[3] = {y + rcr * cr, y - gcb * cb - gcr * cr, y + bcb * cb};
            for (int k = 0; k < 3; k++) {
                float v = c[k] * 255.0f + 0.5f;
                rgb[i * 3 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
        // box filter the DC grid down to the preview
        for (int oy = 0; oy < oh; oy++) {
            int y0 = (int)((long long)oy * img.gh / oh), y1 = (int)((long long)(oy + 1) * img.gh / oh);
            if (y1 <= y0) y1 = y0 + 1;
            for (int ox = 0; ox < ow; ox++) {
                int x0 = (int)((long long)ox * img.gw / ow), x1 = (int)((long long)(ox + 1) * img.gw / ow);
                if (x1 <= x0) x1 = x0 + 1;
                unsigned sum[3] = {0, 0, 0}, cnt = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) {
                        const uint8_t* p = rgb + ((size_t)y * img.gw + x) * 3;
                        sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                        cnt++;
                    }
                uint8_t* o = out + ((size_t)oy * ow + ox) * 4;
                o[0] = (uint8_t)((sum[2] + cnt / 2) / cnt);   // BGRA
                o[1] = (uint8_t)((sum[1] + cnt / 2) / cnt);
                o[2] = (uint8_t)((sum[0] + cnt / 2) / cnt);
                o[3] = 255;
            }
        }
    }
    free(rgb);
    free(img.y);
    free(img.cb);
    free(img.cr);
    if (!ok) {
        free(out);
        return false;
    }
    *out_bgra = out;
    *out_w = ow;
    *out_h = oh;
    return true;
}

bool mov_preview_file(const char* path, int max_w, int max_h,
                      unsigned char** out_bgra, int* out_w, int* out_h) {
    if (!path || !path[0] || !out_bgra) return false;
    *out_bgra = NULL;
    HANDLE f = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = false;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart >= 16 &&
        (unsigned long long)sz.QuadPart < ((size_t)-1) / 2) {
        HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (m) {
            const unsigned char* v = (const unsigned char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            if (v) {
                ok = mov_preview_mem(v, (size_t)sz.QuadPart, max_w, max_h, out_bgra, out_w, out_h);
                UnmapViewOfFile(v);
            }
            CloseHandle(m);
        }
    }
    CloseHandle(f);
    return ok;
}
