// translate.c - display-only translation of file / folder names.
// See include/translate.h for the contract.
//
// Threading: the UI thread calls tr_get() (lookup + enqueue, never blocks
// on the network); one worker thread batches the queued names, fetches
// them over HTTPS and stores the results.  One critical section guards the
// table; entries are allocated once and never freed or moved, so a value
// pointer handed to the UI stays valid for the whole run.
//
// Service: Google Translate's public "gtx" endpoint (no key).  A batch of
// names goes out as one request with one q= per name (each name's language
// is detected on its own); when a batch reply does not line up, each name
// of that batch is fetched on its own instead.

#include "translate.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const TrLang TR_LANGS[] = {
    {"",      "Orig", "Original names"},
    {"en",    "EN",   "English"},
    {"ur",    "UR",   "Urdu"},
    {"zh-CN", "ZH",   "Chinese"},
    {"ar",    "AR",   "Arabic"},
    {"hi",    "HI",   "Hindi"},
    {"es",    "ES",   "Spanish"},
    {"fr",    "FR",   "French"},
    {"de",    "DE",   "German"},
    {"ru",    "RU",   "Russian"},
    {"ja",    "JA",   "Japanese"},
    {"ko",    "KO",   "Korean"},
    {"tr",    "TR",   "Turkish"},
    {"pt",    "PT",   "Portuguese"},
};
const int TR_LANG_COUNT = (int)(sizeof(TR_LANGS) / sizeof(TR_LANGS[0]));

enum { TS_PENDING = 0, TS_INFLIGHT = 1, TS_DONE = 2, TS_FAIL = 3 };

typedef struct {
    int lang;
    char* text;
    char* value;            // DONE: the translation (may equal text)
    volatile LONG state;
    DWORD retry_at;         // FAIL: GetTickCount() after which to retry
    unsigned hash;
} TrEnt;

static CRITICAL_SECTION g_cs;
static bool g_ready = false;
static TrEnt** g_ents = NULL;     // every entry, in insertion order
static int g_n = 0, g_cap = 0;
static int* g_tab = NULL;         // open addressing: index+1, 0 = empty
static int g_tab_cap = 0;
static HANDLE g_evt = NULL;
static void (*g_wake)(void) = NULL;
static volatile LONG g_gen = 0;
static char g_cache_path[1024] = "";

#define TR_RETRY_MS  (2 * 60 * 1000)   // a failed fetch is retried after 2 min
#define TR_BATCH_MAX 40                // names per request
#define TR_BATCH_URL 3000              // encoded query bytes per request

// ---- table ---------------------------------------------------------------
static unsigned tr_hash(int lang, const char* s) {
    unsigned h = 2166136261u ^ (unsigned)lang;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}

static void tab_insert(int idx) {
    unsigned m = (unsigned)g_tab_cap - 1;
    unsigned i = g_ents[idx]->hash & m;
    while (g_tab[i]) i = (i + 1) & m;
    g_tab[i] = idx + 1;
}

static bool tab_grow(void) {
    int nc = g_tab_cap ? g_tab_cap * 2 : 1024;
    int* nt = (int*)calloc((size_t)nc, sizeof(int));
    if (!nt) return false;
    free(g_tab);
    g_tab = nt;
    g_tab_cap = nc;
    for (int k = 0; k < g_n; k++) tab_insert(k);
    return true;
}

static TrEnt* tab_find(int lang, const char* text, unsigned h) {
    if (!g_tab_cap) return NULL;
    unsigned m = (unsigned)g_tab_cap - 1;
    for (unsigned i = h & m; g_tab[i]; i = (i + 1) & m) {
        TrEnt* e = g_ents[g_tab[i] - 1];
        if (e->hash == h && e->lang == lang && strcmp(e->text, text) == 0)
            return e;
    }
    return NULL;
}

// Caller holds g_cs.
static TrEnt* tab_add(int lang, const char* text, unsigned h) {
    if ((g_n + 1) * 2 > g_tab_cap && !tab_grow()) return NULL;
    if (g_n == g_cap) {
        int nc = g_cap ? g_cap * 2 : 512;
        TrEnt** na = (TrEnt**)realloc(g_ents, sizeof(TrEnt*) * (size_t)nc);
        if (!na) return NULL;
        g_ents = na;
        g_cap = nc;
    }
    TrEnt* e = (TrEnt*)calloc(1, sizeof(TrEnt));
    if (!e) return NULL;
    e->text = _strdup(text);
    if (!e->text) { free(e); return NULL; }
    e->lang = lang;
    e->hash = h;
    g_ents[g_n] = e;
    tab_insert(g_n);
    g_n++;
    return e;
}

// ---- cache file ----------------------------------------------------------
static void clean_field(char* s) {
    for (; *s; s++)
        if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

static void cache_load(void) {
    FILE* f = fopen(g_cache_path, "rb");
    if (!f) return;
    char line[2200];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char* t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char* t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = 0;
        int lang = -1;
        for (int k = 1; k < TR_LANG_COUNT; k++)
            if (strcmp(TR_LANGS[k].code, line) == 0) { lang = k; break; }
        if (lang < 0 || !t1[1] || !t2[1]) continue;
        unsigned h = tr_hash(lang, t1 + 1);
        TrEnt* e = tab_find(lang, t1 + 1, h);
        if (!e) e = tab_add(lang, t1 + 1, h);
        if (!e || e->value) continue;
        e->value = _strdup(t2 + 1);
        e->state = e->value ? TS_DONE : TS_FAIL;
    }
    fclose(f);
}

static void cache_append(TrEnt** done, int n) {
    if (!g_cache_path[0] || n <= 0) return;
    FILE* f = fopen(g_cache_path, "ab");
    if (!f) return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\t%s\t%s\n", TR_LANGS[done[i]->lang].code,
                done[i]->text, done[i]->value);
    fclose(f);
}

// ---- HTTPS ---------------------------------------------------------------
static void url_encode(const char* s, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

// GET https://translate.googleapis.com<path>; returns a malloc'd,
// NUL-terminated body or NULL.
static char* https_get(const char* path) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wn <= 0) return NULL;
    WCHAR* wpath = (WCHAR*)malloc(sizeof(WCHAR) * (size_t)wn);
    if (!wpath) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wn);

    char* body = NULL;
    size_t len = 0, cap = 0;
    bool ok = false;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    ses = WinHttpOpen(L"NovaFile", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 5000, 8000, 8000, 10000);
        con = WinHttpConnect(ses, L"translate.googleapis.com",
                             INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, sl = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                     WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
                            WINHTTP_NO_HEADER_INDEX);
        ok = (status == 200);
        while (ok) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
            if (avail == 0) break;
            if (len + avail + 1 > cap) {
                size_t nc = (len + avail + 1) * 2;
                if (nc > (4u << 20)) { ok = false; break; }   // 4 MB cap
                char* nb = (char*)realloc(body, nc);
                if (!nb) { ok = false; break; }
                body = nb;
                cap = nc;
            }
            DWORD got = 0;
            if (!WinHttpReadData(req, body + len, avail, &got)) {
                ok = false;
                break;
            }
            len += got;
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(wpath);
    if (!ok || !body) { free(body); return NULL; }
    body[len] = 0;
    return body;
}

// ---- reply parsing -------------------------------------------------------
// Reply shape: [[["translated","original",...],["...","...",...],...],...]
// Only the first string of every segment of the first array is needed.
static const char* ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static void put_utf8(unsigned cp, char** o, char* end) {
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63));
        b[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63));
        b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63));
        n = 4;
    }
    if (*o + n >= end) return;
    memcpy(*o, b, (size_t)n);
    *o += n;
}

static unsigned hex4(const char* p, bool* ok) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else { *ok = false; return 0; }
    }
    return v;
}

// p at the opening quote.  Decodes into out (when non-NULL) and returns the
// position after the closing quote, or NULL on malformed input.
static const char* json_str(const char* p, char** out, char* end) {
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            char c = *p;
            if (!c) return NULL;
            unsigned cp = 0;
            bool ok = true;
            switch (c) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u':
                cp = hex4(p + 1, &ok);
                if (!ok) return NULL;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                    unsigned lo = hex4(p + 3, &ok);
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                break;
            default: cp = (unsigned char)c; break;
            }
            if (out) put_utf8(cp, out, end);
            p++;
        } else {
            if (out && *out + 1 < end) *(*out)++ = *p;
            p++;
        }
    }
    if (*p != '"') return NULL;
    return p + 1;
}

static const char* json_skip(const char* p) {
    p = ws(p);
    if (*p == '"') return json_str(p, NULL, NULL);
    if (*p == '[' || *p == '{') {
        char close = (*p == '[') ? ']' : '}';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            p = json_skip(p);
            if (!p) return NULL;
            p = ws(p);
            if (*p == ':') { p = json_skip(p + 1); if (!p) return NULL; p = ws(p); }
            if (*p == ',') { p++; continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    while (*p && *p != ',' && *p != ']' && *p != '}') p++;   // number/null/bool
    return *p ? p : NULL;
}

// Reply of /translate_a/t with several q= values: one element per q, in
// order - either "translation" or ["translation","detected-language"].
// Fills results[0..n); false when the reply does not have exactly n items.
static bool parse_reply(const char* body, char** results, int n) {
    size_t cap = strlen(body) + 16;
    const char* p = ws(body);
    int got = 0;
    if (*p == '"' && n == 1) {               // a lone string reply
        char* buf = (char*)malloc(cap);
        if (!buf) return false;
        char* o = buf;
        if (!json_str(p, &o, buf + cap - 1)) { free(buf); return false; }
        *o = 0;
        results[0] = buf;
        return true;
    }
    if (*p != '[') return false;
    p = ws(p + 1);
    while (*p && *p != ']') {
        char* buf = (char*)malloc(cap);
        if (!buf) goto bad;
        char* o = buf;
        if (*p == '"') {
            p = json_str(p, &o, buf + cap - 1);
        } else if (*p == '[') {
            p = ws(p + 1);
            if (*p == '"') p = json_str(p, &o, buf + cap - 1);
            while (p && *p && *p != ']') {   // detected language etc.
                p = ws(p);
                if (*p == ',') { p++; continue; }
                if (*p == ']') break;
                p = json_skip(p);
            }
            if (p && *p == ']') p++;
        } else {
            p = json_skip(p);                 // null: no translation
        }
        if (!p) { free(buf); goto bad; }
        *o = 0;
        if (got < n) results[got] = buf;
        else free(buf);
        got++;
        p = ws(p);
        if (*p == ',') p = ws(p + 1);
    }
    if (got == n) return true;
bad:
    for (int i = 0; i < n && i < got; i++) { free(results[i]); results[i] = NULL; }
    return false;
}

static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

// Translate texts[0..n) into lang; results[i] = malloc'd string or NULL.
// Every name is its own q= value, so the service detects each name's
// language separately (a folder can mix Chinese, Urdu, Russian... names).
static bool fetch_batch(int lang, char** texts, int n, char** results) {
    size_t qcap = 256;
    for (int i = 0; i < n; i++) qcap += strlen(texts[i]) * 3 + 8;
    char* q = (char*)malloc(qcap);
    if (!q) return false;
    size_t o = (size_t)snprintf(q, 256, "/translate_a/t?client=gtx&sl=auto&tl=%s",
                                TR_LANGS[lang].code);
    for (int i = 0; i < n; i++) {
        memcpy(q + o, "&q=", 3);
        o += 3;
        url_encode(texts[i], q + o, qcap - o);
        o += strlen(q + o);
    }
    char* body = https_get(q);
    free(q);
    if (!body) return false;
    bool ok = parse_reply(body, results, n);
    free(body);
    if (!ok) return false;
    for (int i = 0; i < n; i++)
        if (results[i]) { clean_field(results[i]); trim(results[i]); }
    return true;
}

// ---- worker --------------------------------------------------------------
static DWORD WINAPI tr_worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_evt, INFINITE);
        for (;;) {
            TrEnt* batch[TR_BATCH_MAX];
            char* texts[TR_BATCH_MAX];
            int n = 0, lang = -1;
            size_t bytes = 0;
            EnterCriticalSection(&g_cs);
            for (int k = 0; k < g_n && n < TR_BATCH_MAX; k++) {
                TrEnt* e = g_ents[k];
                if (e->state != TS_PENDING) continue;
                if (lang < 0) lang = e->lang;
                if (e->lang != lang) continue;
                size_t l = strlen(e->text) * 3 + 3;
                if (n > 0 && bytes + l > TR_BATCH_URL) break;
                bytes += l;
                e->state = TS_INFLIGHT;
                batch[n] = e;
                texts[n] = e->text;
                n++;
            }
            LeaveCriticalSection(&g_cs);
            if (n == 0) break;

            char* res[TR_BATCH_MAX];
            memset(res, 0, sizeof res);
            bool ok = fetch_batch(lang, texts, n, res);
            if (!ok && n > 1) {          // one name per request instead
                ok = true;
                for (int i = 0; i < n; i++) {
                    char* one[1] = {NULL};
                    if (fetch_batch(lang, &texts[i], 1, one)) res[i] = one[0];
                    else ok = false;
                }
            }
            TrEnt* done[TR_BATCH_MAX];
            int nd = 0;
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < n; i++) {
                if (res[i] && res[i][0]) {
                    batch[i]->value = res[i];
                    InterlockedExchange(&batch[i]->state, TS_DONE);
                    done[nd++] = batch[i];
                } else {
                    free(res[i]);
                    batch[i]->retry_at = GetTickCount() + TR_RETRY_MS;
                    InterlockedExchange(&batch[i]->state, TS_FAIL);
                }
            }
            LeaveCriticalSection(&g_cs);
            cache_append(done, nd);
            if (nd > 0) {
                InterlockedIncrement(&g_gen);
                if (g_wake) g_wake();
            }
            if (!ok) Sleep(1500);        // network trouble: do not hammer
        }
    }
    return 0;
}

// ---- public --------------------------------------------------------------
void tr_init(const char* cache_dir, void (*wake)(void)) {
    if (g_ready) return;
    InitializeCriticalSection(&g_cs);
    g_wake = wake;
    if (cache_dir && cache_dir[0]) {
        CreateDirectoryA(cache_dir, NULL);
        snprintf(g_cache_path, sizeof g_cache_path, "%s\\translations.txt",
                 cache_dir);
        cache_load();
    }
    g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_evt) return;
    HANDLE th = CreateThread(NULL, 0, tr_worker, NULL, 0, NULL);
    if (!th) return;
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
    g_ready = true;
}

bool tr_needed(const char* text, int lang) {
    if (!text || !text[0] || lang <= 0 || lang >= TR_LANG_COUNT) return false;
    bool any_alpha = false;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p >= 0x80) return true;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
            any_alpha = true;
    }
    // plain ASCII: already English; other targets translate real words
    return lang != TR_LANG_ENGLISH && any_alpha;
}

const char* tr_get(const char* text, int lang) {
    if (!g_ready || !tr_needed(text, lang)) return NULL;
    if (strlen(text) > 1000) return NULL;
    unsigned h = tr_hash(lang, text);
    const char* out = NULL;
    bool kick = false;
    EnterCriticalSection(&g_cs);
    TrEnt* e = tab_find(lang, text, h);
    if (!e) {
        e = tab_add(lang, text, h);
        kick = (e != NULL);
    } else if (e->state == TS_DONE) {
        out = e->value;
    } else if (e->state == TS_FAIL &&
               (LONG)(GetTickCount() - e->retry_at) >= 0) {
        e->state = TS_PENDING;
        kick = true;
    }
    LeaveCriticalSection(&g_cs);
    if (kick) SetEvent(g_evt);
    return out;
}

unsigned tr_generation(void) {
    return (unsigned)InterlockedCompareExchange(&g_gen, 0, 0);
}
