// demo_record.c - scripted, invisible product-video recorder.
//
// Compiled ONLY into a separate build (-DNOVA_DEMO, #included by main.c);
// the shipped app contains none of it.  Run with NOVA_OFFSCREEN=1 so the
// window is parked beyond every monitor and never takes focus: the user's
// mouse, keyboard, clipboard and screen are never touched.  Each frame the
// script writes the InputState the app reads (pointer, buttons, keys,
// typed text), the frame renders as usual, and the finished back buffer is
// piped to ffmpeg (started with no console window).  A per-frame log
// (pointer, button, events) lets the editor draw the cursor, click rings
// and key overlays afterwards.
//
//   NOVA_DEMO_SCRIPT  script file        NOVA_DEMO_OUT   output .mp4
//   NOVA_DEMO_FFMPEG  ffmpeg.exe         NOVA_DEMO_LOG   per-frame log
//   NOVA_DEMO_SNAP    folder for "snap" stills (layout planning)
//
// Script: one command per line, '#' comments.  Coordinates are window
// pixels.  Durations are frames (30 per second).
//   wait N | move X Y N | click | dclick | rclick | mclick | down | up
//   key SPEC | keydown SPEC | keyup SPEC   (SPEC: ctrl+shift+n, alt+left,
//   f2, enter, esc, tab, pgup, comma, apps, a..z, 0..9 ...)
//   mods SPEC|none (held modifiers for clicks / wheel) | wheel N
//   type TEXT | xbutton 1|2 | mark NAME | snap NAME | end

typedef struct {
    bool on;
    char** lines;
    int nlines, pc;
    int wait;                 // frames left on the current command
    // glide
    int gx0, gy0, gx1, gy1, gn, gi;
    int x, y;
    bool held;                // left button held (down/up, drags)
    int mods;                 // persistent M_CTRL | M_SHIFT | M_ALT
    // multi-frame actions
    int phase;                // click / dclick / key sequencing
    int act;                  // 1 click 2 dclick 3 key 4 type
    int key_vk, key_mods;
    char text[256];
    int tpos;
    long frame;
    FILE* log;
    HANDLE pipe_w, ff_proc;
    int fw, fh;
    unsigned char* buf;
    char snapdir[512];
    char evt[160];            // this frame's event label (log)
} Demo;
static Demo g_demo;

static int demo_vk(const char* s) {
    if (!s[0]) return 0;
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') return c - 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    static const struct { const char* n; int vk; } T[] = {
        {"enter", VK_RETURN}, {"esc", VK_ESCAPE}, {"tab", VK_TAB},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN},
        {"pgup", VK_PRIOR}, {"pgdn", VK_NEXT}, {"home", VK_HOME}, {"end", VK_END},
        {"delete", VK_DELETE}, {"backspace", VK_BACK}, {"space", VK_SPACE},
        {"comma", VK_OEM_COMMA}, {"apps", VK_APPS}, {"plus", VK_OEM_PLUS},
        {"minus", VK_OEM_MINUS},
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4}, {"f5", VK_F5},
        {"f10", VK_F10}, {"f11", VK_F11}, {NULL, 0}};
    for (int i = 0; T[i].n; i++)
        if (!_stricmp(s, T[i].n)) return T[i].vk;
    return 0;
}

// "ctrl+shift+n" -> vk + mods
static int demo_spec(const char* spec, int* mods) {
    char b[64];
    snprintf(b, sizeof b, "%s", spec);
    *mods = 0;
    int vk = 0;
    for (char* t = strtok(b, "+"); t; t = strtok(NULL, "+")) {
        if (!_stricmp(t, "ctrl")) *mods |= M_CTRL;
        else if (!_stricmp(t, "shift")) *mods |= M_SHIFT;
        else if (!_stricmp(t, "alt")) *mods |= M_ALT;
        else if (!_stricmp(t, "none")) *mods = 0;
        else vk = demo_vk(t);
    }
    return vk;
}

static void demo_spawn_ffmpeg(int w, int h) {
    const char* ff = getenv("NOVA_DEMO_FFMPEG");
    const char* out = getenv("NOVA_DEMO_OUT");
    if (!ff || !out) return;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 22)) return;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);   // only ffmpeg's end
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "\"%s\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgra "
             "-s %dx%d -r 30 -i - -c:v libx264 -preset veryfast -crf 12 "
             "-pix_fmt yuv420p \"%s\"", ff, w, h, out);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        g_demo.ff_proc = pi.hProcess;
        g_demo.pipe_w = wr;
    } else {
        CloseHandle(wr);
    }
    CloseHandle(rd);
}

static bool demo_init(void) {
    const char* sp = getenv("NOVA_DEMO_SCRIPT");
    if (!sp) return false;
    FILE* f = fopen(sp, "rb");
    if (!f) return false;
    char line[600];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        if (g_demo.nlines == cap) {
            cap = cap ? cap * 2 : 256;
            g_demo.lines = (char**)realloc(g_demo.lines, sizeof(char*) * (size_t)cap);
        }
        g_demo.lines[g_demo.nlines++] = _strdup(p);
    }
    fclose(f);
    const char* lp = getenv("NOVA_DEMO_LOG");
    if (lp) g_demo.log = fopen(lp, "wb");
    const char* sd = getenv("NOVA_DEMO_SNAP");
    snprintf(g_demo.snapdir, sizeof g_demo.snapdir, "%s", sd ? sd : ".");
    g_demo.x = 960;
    g_demo.y = 540;
    g_demo.on = true;
    g_clip_quiet = true;      // never touch the user's clipboard
    return true;
}

static void demo_save_bmp(const char* path, const unsigned char* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bi;
    memset(&bf, 0, sizeof bf);
    memset(&bi, 0, sizeof bi);
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof bf + sizeof bi;
    bf.bfSize = bf.bfOffBits + (DWORD)w * h * 4;
    bi.biSize = sizeof bi;
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    fwrite(&bf, sizeof bf, 1, f);
    fwrite(&bi, sizeof bi, 1, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

static float demo_ease(float t) { return t * t * (3.0f - 2.0f * t); }

// One frame of scripted input.  False = the script finished.
static bool demo_tick(InputState* in) {
    Demo* d = &g_demo;
    d->evt[0] = 0;
    // the app reads ONLY what the script says: no real pointer, no real keys
    in->left_click = in->right_click = in->middle_click = in->double_click = false;
    in->key_code = in->key_repeat = in->key_up = 0;
    in->char_pressed = 0;
    in->charq_n = 0;
    in->wheel = 0;
    in->xbutton = 0;
    in->v_held = false;
    in->win_pressed = false;
    int fm = d->mods;           // modifiers for this frame

    for (;;) {
        if (d->wait > 0) {
            d->wait--;
            if (d->gn > 0) {
                d->gi++;
                float t = demo_ease((float)d->gi / (float)d->gn);
                d->x = d->gx0 + (int)((d->gx1 - d->gx0) * t);
                d->y = d->gy0 + (int)((d->gy1 - d->gy0) * t);
                if (d->gi >= d->gn) d->gn = 0;
            }
            if (d->act == 1 || d->act == 2) {           // click / dclick
                int ph = d->phase++;
                bool press = (ph == 0) || (d->act == 2 && ph == 3);
                if (press) {
                    in->left_click = true;
                    if (d->act == 2 && ph == 3) in->double_click = true;
                }
                in->left_down = press;
                if (d->wait == 0) d->act = 0;
            } else if (d->act == 3) {                   // key: press, release
                if (d->phase++ == 0) {
                    in->key_code = d->key_vk;
                    fm |= d->key_mods;
                } else {
                    in->key_up = d->key_vk;
                    fm |= d->key_mods;
                    d->act = 0;
                }
            } else if (d->act == 4) {                   // typing, 1 char / 2 frames
                if ((d->phase++ & 1) == 0 && d->text[d->tpos]) {
                    unsigned c = (unsigned char)d->text[d->tpos++];
                    in->char_pressed = c;
                    in->charq[0] = c;
                    in->charq_n = 1;
                }
                if (d->wait == 0) d->act = 0;
            }
            break;
        }
        if (d->pc >= d->nlines) return false;
        char cmd[600];
        snprintf(cmd, sizeof cmd, "%s", d->lines[d->pc++]);
        char* arg = strchr(cmd, ' ');
        if (arg) *arg++ = 0; else arg = cmd + strlen(cmd);
        if (!strcmp(cmd, "wait")) {
            d->wait = atoi(arg);
        } else if (!strcmp(cmd, "move")) {
            int x = 0, y = 0, n = 1;
            sscanf(arg, "%d %d %d", &x, &y, &n);
            if (n < 1) n = 1;
            d->gx0 = d->x; d->gy0 = d->y; d->gx1 = x; d->gy1 = y;
            d->gn = n; d->gi = 0; d->wait = n;
        } else if (!strcmp(cmd, "click")) {
            d->act = 1; d->phase = 0; d->wait = 2;
            snprintf(d->evt, sizeof d->evt, "click");
        } else if (!strcmp(cmd, "dclick")) {
            d->act = 2; d->phase = 0; d->wait = 5;
            snprintf(d->evt, sizeof d->evt, "dclick");
        } else if (!strcmp(cmd, "rclick")) {
            in->right_click = true; d->wait = 1;
            snprintf(d->evt, sizeof d->evt, "rclick");
        } else if (!strcmp(cmd, "mclick")) {
            in->middle_click = true; d->wait = 1;
            snprintf(d->evt, sizeof d->evt, "mclick");
        } else if (!strcmp(cmd, "down")) {
            d->held = true; in->left_click = true; d->wait = 1;
            snprintf(d->evt, sizeof d->evt, "down");
        } else if (!strcmp(cmd, "up")) {
            d->held = false; d->wait = 1;
            snprintf(d->evt, sizeof d->evt, "up");
        } else if (!strcmp(cmd, "key")) {
            // SAFETY: a key that changes files may only run inside the demo
            // folder (NOVA_DEMO_ROOT).  If the script ever wandered out, the
            // recording stops instead of touching anything else.
            static const char* const risky[] = {"ctrl+x", "ctrl+v", "delete", "shift+delete", "f2",
                                                "ctrl+shift+n", "ctrl+z", NULL};
            const char* root = getenv("NOVA_DEMO_ROOT");
            for (int r = 0; risky[r]; r++) {
                if (_stricmp(arg, risky[r]) != 0) continue;
                size_t rl = root ? strlen(root) : 0;
                bool inside = rl > 0 && _strnicmp(g_app.current_path, root, rl) == 0 &&
                              (g_app.current_path[rl] == '\0' || g_app.current_path[rl] == '\\');
                if (!inside) {
                    if (d->log) fprintf(d->log, "%ld GUARD %s blocked in %s\n", d->frame, arg,
                                        g_app.current_path);
                    return false;
                }
            }
            d->key_vk = demo_spec(arg, &d->key_mods);
            d->act = 3; d->phase = 0; d->wait = 2;
            snprintf(d->evt, sizeof d->evt, "key %s", arg);
        } else if (!strcmp(cmd, "keydown")) {
            int m = 0;
            in->key_code = demo_spec(arg, &m);
            fm |= m; d->wait = 1;
            snprintf(d->evt, sizeof d->evt, "keydown %s", arg);
        } else if (!strcmp(cmd, "keyup")) {
            int m = 0;
            in->key_up = demo_spec(arg, &m);
            d->wait = 1;
        } else if (!strcmp(cmd, "mods")) {
            demo_spec(arg, &d->mods);
            fm = d->mods;
            continue;
        } else if (!strcmp(cmd, "wheel")) {
            in->wheel = atoi(arg); d->wait = 1;
            snprintf(d->evt, sizeof d->evt, "wheel %s", arg);
        } else if (!strcmp(cmd, "xbutton")) {
            in->xbutton = atoi(arg); d->wait = 1;
        } else if (!strcmp(cmd, "type")) {
            snprintf(d->text, sizeof d->text, "%s", arg);
            d->tpos = 0; d->act = 4; d->phase = 0;
            d->wait = (int)strlen(d->text) * 2 + 1;
            snprintf(d->evt, sizeof d->evt, "type %s", arg);
        } else if (!strcmp(cmd, "mark")) {
            if (d->log) fprintf(d->log, "%ld mark %s\n", d->frame, arg);
            continue;
        } else if (!strcmp(cmd, "snap")) {
            if (d->buf && d->fw > 0) {
                char p[700];
                snprintf(p, sizeof p, "%s\\%s.bmp", d->snapdir, arg);
                demo_save_bmp(p, d->buf, d->fw, d->fh);
            }
            continue;
        } else if (!strcmp(cmd, "end")) {
            return false;
        } else {
            continue;                                   // unknown: skip
        }
        // the first frame of a timed command is this one
        d->wait++;
    }
    in->x = d->x;
    in->y = d->y;
    if (d->act != 1 && d->act != 2) in->left_down = d->held;
    in->ctrl_pressed = (fm & M_CTRL) != 0;
    in->shift_pressed = (fm & M_SHIFT) != 0;
    in->alt_pressed = (fm & M_ALT) != 0;
    return true;
}

// After paint(): the finished back buffer goes to ffmpeg, the frame's
// pointer / button / event to the log.
static void demo_capture(void) {
    Demo* d = &g_demo;
    int w = 0, h = 0;
    HBITMAP bb = (HBITMAP)renderer_last_frame(&w, &h);
    if (!bb || w <= 0 || h <= 0) return;
    if (!d->buf) {
        d->fw = w & ~1;
        d->fh = h & ~1;
        d->buf = (unsigned char*)malloc((size_t)w * h * 4);
        demo_spawn_ffmpeg(d->fw, d->fh);
    }
    if (w < d->fw || h < d->fh) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);
    GetDIBits(dc, bb, 0, (UINT)h, d->buf, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (d->pipe_w) {
        for (int y = 0; y < d->fh; y++) {
            DWORD wr = 0;
            WriteFile(d->pipe_w, d->buf + (size_t)y * w * 4, (DWORD)d->fw * 4, &wr, NULL);
        }
    }
    if (d->log)
        fprintf(d->log, "%ld %d %d %d %s\n", d->frame, d->x, d->y,
                (d->held || d->act == 1 || d->act == 2) ? 1 : 0, d->evt);
    d->frame++;
}

static void demo_finish(void) {
    Demo* d = &g_demo;
    if (d->pipe_w) { CloseHandle(d->pipe_w); d->pipe_w = NULL; }
    if (d->ff_proc) {
        WaitForSingleObject(d->ff_proc, 120000);
        CloseHandle(d->ff_proc);
        d->ff_proc = NULL;
    }
    if (d->log) { fclose(d->log); d->log = NULL; }
}
