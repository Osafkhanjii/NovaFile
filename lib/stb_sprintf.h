// Minimal stb_sprintf implementation stub
// In production, use: https://github.com/nothings/stb/blob/master/stb_sprintf.h
#pragma once
#include <stdio.h>

static int stb_sprintf(char* buf, int buf_max, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, buf_max, fmt, args);
    va_end(args);
    return ret;
}
