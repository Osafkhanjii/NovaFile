// video_thumb.h - one video frame through Windows Media Foundation.
//
// The fallback for videos the shell has no thumbnail for (some PCs never
// register a thumbnail handler for .mov even though Windows can decode
// the H.264 inside).  Media Foundation is loaded at run time, so the app
// still starts on Windows "N" editions that ship without it - there this
// simply returns false.
#ifndef NOVA_VIDEO_THUMB_H
#define NOVA_VIDEO_THUMB_H

#include <stdbool.h>

// True for the video names this route is tried on (.mov .mp4 .m4v).
bool video_is_video_name(const char* name);

// A frame ~10% into the video, scaled to fit max_w x max_h (aspect
// kept).  *out_bgra: malloc'ed opaque top-down BGRA, the caller frees.
// Call from a thread that has initialised COM.
bool video_frame_preview(const char* path, int max_w, int max_h,
                         unsigned char** out_bgra, int* out_w, int* out_h);

#endif // NOVA_VIDEO_THUMB_H
