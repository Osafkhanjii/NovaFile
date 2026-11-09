// mov.h - fast thumbnails for QuickTime .mov files.
//
// Windows ships no Apple ProRes codec, so Explorer (and the shell
// thumbnail route NovaFile uses for videos) shows a blank icon for every
// ProRes .mov - the standard delivery format in VFX / editing.  NovaFile
// reads the container itself and decodes ONE frame of a ProRes track
// (every profile: Proxy, LT, 422, HQ, 4444, 4444 XQ; progressive or
// interlaced) from its 8x8 block averages alone - a 1/8 resolution
// picture, in a few milliseconds even for 4K.  Pure C, no libraries.
//
// Other codecs (H.264, HEVC, ...) return false: the caller then uses the
// shell, and Media Foundation after that.
#ifndef NOVA_MOV_H
#define NOVA_MOV_H

#include <stdbool.h>
#include <stddef.h>

// True for a ".mov" file name (case-insensitive).
bool mov_is_mov_name(const char* name);

// Decode a preview of the ProRes video track that fits max_w x max_h
// (aspect kept, never larger than 1/8 of the frame).  On success
// *out_bgra is a malloc'ed, opaque, top-down BGRA buffer the caller
// frees.  False = not a ProRes .mov, or a damaged one.
bool mov_preview_mem(const unsigned char* data, size_t size, int max_w, int max_h,
                     unsigned char** out_bgra, int* out_w, int* out_h);
bool mov_preview_file(const char* path, int max_w, int max_h,
                      unsigned char** out_bgra, int* out_w, int* out_h);

#endif // NOVA_MOV_H
