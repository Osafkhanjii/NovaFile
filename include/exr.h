// exr.h - OpenEXR preview decoder (src/core/exr.c).
//
// Windows ships no codec for .exr, so the shell hands back a blank file
// icon for every frame of an image sequence. NovaFile decodes the file
// itself - pure C, no libraries (zlib inflate, PIZ, PXR24, B44 and DWA
// are implemented in exr.c) - and turns it into a small sRGB preview.
//
// Supported: scanline and tiled images, single part or the first part of
// a multi-part file, compression NONE / RLE / ZIPS / ZIP / PIZ / PXR24 /
// B44 / B44A / DWAA / DWAB, HALF / FLOAT / UINT channels. Deep images
// are refused (false) and keep the normal icon.
#ifndef NOVA_EXR_H
#define NOVA_EXR_H

#include <stdbool.h>
#include <stddef.h>

// True when the file name ends in ".exr" (any case).
bool exr_is_exr_name(const char* name);

// Decode an EXR held in memory into an opaque 8-bit BGRA preview (top-down
// rows, 4 bytes per pixel, alpha = 255) that fits inside max_w x max_h
// with its aspect kept and is never larger than the image itself.
// Transparent pixels are composited over bg (0xRRGGBB). On success
// *out_bgra is malloc'd (caller frees) and the size is in *out_w/*out_h.
bool exr_preview_mem(const unsigned char* data, size_t size,
                     int max_w, int max_h, unsigned bg,
                     unsigned char** out_bgra, int* out_w, int* out_h);

// Same, for a file on disk (path in the process code page - UTF-8 here).
// The file is memory-mapped read-only and never written.
bool exr_preview_file(const char* path, int max_w, int max_h, unsigned bg,
                      unsigned char** out_bgra, int* out_w, int* out_h);

#endif // NOVA_EXR_H
