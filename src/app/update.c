// update.c - NovaFile auto-update client.  Pure C, Windows system DLLs
// only (winhttp, bcrypt, crypt32, wintrust): the app's "no external
// dependencies" rule stays intact because all four ship with Windows.
//
// Flow (one detached worker, results published under a critical section
// and polled per frame - the same house style as the thumbnail workers):
//
//   check manifest (strict flat JSON, https only)
//       -> parse + schema check
//       -> compare versions (equal/older = up to date: no downgrade)
//       -> download package to <appdir>\updates\*.exe.part
//       -> verify size + SHA-256 (+ Authenticode when signer configured)
//       -> rename to the final name, state = READY
//
// The updater NEVER executes a package that failed verification, never
// follows a URL that is not https (loopback http is allowed for the
// offline test rig only), and never touches the running exe itself - the
// verified installer replaces files after this process exits.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "version.h"
#include "update.h"

// ----------------------------------------------------------------- config
#define UP_CFG_NAME      "updates.cfg"      // <appdir>\updates.cfg
#define UP_DIR_NAME      "updates"          // <appdir>\updates\ (packages)
#define UP_MANIFEST_MAX  (64 * 1024)        // manifest hard cap
#define UP_PACKAGE_MAX   (400LL << 20)      // package sanity cap: 400 MB
#define UP_CONNECT_MS    5000
#define UP_READ_MS       15000

// ------------------------------------------------------------------ state
enum {
    UPS_IDLE = 0, UPS_BUSY, UPS_UPTODATE, UPS_READY, UPS_ERROR,
    UPS_NOT_CONFIGURED, UPS_INSECURE
};

static CRITICAL_SECTION g_up_cs;            // guards every g_up_* string
static bool             g_up_cs_ready = false;
static volatile LONG    g_up_state = UPS_IDLE;
static char g_up_status[256];               // worker -> pump
static char g_up_show[256];                 // pump -> settings row (stable)
static char g_up_ver[32];                   // latest offered version
static char g_up_url[640];                  // manifest url (validated)
static char g_up_sha[80];                   // expected package SHA-256
static char g_up_notes[640];                // release-notes URL (optional)
static char g_up_minv[32];                  // min compatible version
static char g_up_signer[80];                // signer thumbprint (optional)
static char g_up_pkg[MAX_PATH];             // verified package path
static long long g_up_size = 0;             // expected bytes (0 = unknown)
static long long g_up_got = 0;              // downloaded bytes
static char g_up_cfg_url[640];              // configured manifest URL
static bool g_up_cfg_reqsig = false;        // require_signature=1
static bool g_up_cfg_read = false;
static HANDLE g_up_thread = NULL;
static bool g_up_echo = false;              // CLI: stream progress to stdout

// ------------------------------------------------------------- tiny JSON
// Strict scanner for the flat manifest schema.  Unknown keys are skipped,
// nested structures are rejected, duplicate keys are first-wins (the same
// policy settings.txt uses).  No \uXXXX - every value in the schema is
// ASCII, so accepting escapes would only widen the attack surface.
static const char* jskip(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static bool jstring(const char** pp, char* out, size_t cap) {
    const char* p = *pp;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    for (;;) {
        char c = *p;
        if (c == '"') { p++; break; }
        if (c == '\0' || (unsigned char)c < 0x20) return false;
        if (c == '\\') {
            char e = p[1];
            if (e == '"' || e == '\\' || e == '/') c = e;
            else return false;               // \u, \n, \t... not in schema
            p += 2;
        } else {
            p++;
        }
        if (n + 1 >= cap) return false;      // oversized value = reject
        out[n++] = c;
    }
    out[n] = '\0';
    *pp = p;
    return true;
}

bool nova_manifest_parse(const char* json,
                         char* ver, size_t vn, char* url, size_t un,
                         char* sha, size_t sn, long long* size,
                         char* notes, size_t non, char* minv, size_t mn,
                         char* signer, size_t sgn) {
    if (!json || !ver || !url || !sha || !size || !notes || !minv || !signer)
        return false;
    ver[0] = url[0] = sha[0] = notes[0] = minv[0] = signer[0] = '\0';
    *size = 0;
    bool size_seen = false;

    const char* p = jskip(json);
    if (*p != '{') return false;
    p = jskip(p + 1);
    if (*p == '}') return false;             // empty object = invalid

    for (;;) {
        char key[64], val[640];
        if (!jstring(&p, key, sizeof key)) return false;
        p = jskip(p);
        if (*p != ':') return false;
        p = jskip(p + 1);
        if (*p == '"') {
            if (!jstring(&p, val, sizeof val)) return false;
            // first-wins
            if (strcmp(key, "version") == 0)  { if (!ver[0])    snprintf(ver, vn, "%s", val); }
            else if (strcmp(key, "url") == 0) { if (!url[0])    snprintf(url, un, "%s", val); }
            else if (strcmp(key, "sha256") == 0) { if (!sha[0]) snprintf(sha, sn, "%s", val); }
            else if (strcmp(key, "notes") == 0) { if (!notes[0]) snprintf(notes, non, "%s", val); }
            else if (strcmp(key, "min_version") == 0) { if (!minv[0]) snprintf(minv, mn, "%s", val); }
            else if (strcmp(key, "signer") == 0) { if (!signer[0]) snprintf(signer, sgn, "%s", val); }
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char* end = NULL;
            long long num = strtoll(p, &end, 10);
            if (end == p) return false;
            if (strcmp(key, "size") == 0 && !size_seen) {
                *size = num;
                size_seen = true;
            }
            p = end;
        } else {
            return false;                    // objects/arrays/bools/null
        }
        p = jskip(p);
        if (*p == ',') {
            p = jskip(p + 1);
            if (*p == '}') return false;     // trailing comma
            continue;
        }
        if (*p == '}') { p++; break; }
        return false;
    }
    if (*jskip(p) != '\0') return false;     // one object only

    // schema gate: everything the client acts on must be present and sane
    char schema[16] = "", product[64] = "";
    // re-scan cheaply: the values above are already extracted; schema and
    // product are checked here by a second strict pass over the same text
    {
        const char* q = jskip(json);
        if (*q != '{') return false;
        q = jskip(q + 1);
        for (;;) {
            char k2[64], v2[640];
            q = jskip(q);
            if (*q != '"') return false;
            if (!jstring(&q, k2, sizeof k2)) return false;
            q = jskip(q);
            if (*q != ':') return false;
            q = jskip(q + 1);
            if (*q == '"') {
                if (!jstring(&q, v2, sizeof v2)) return false;
                if (strcmp(k2, "schema") == 0 && !schema[0])
                    snprintf(schema, sizeof schema, "%s", v2);
                else if (strcmp(k2, "product") == 0 && !product[0])
                    snprintf(product, sizeof product, "%s", v2);
            } else if (*q == '-' || (*q >= '0' && *q <= '9')) {
                char* end = NULL;
                strtoll(q, &end, 10);
                if (end == q) return false;
                q = end;
            } else return false;
            q = jskip(q);
            if (*q == ',') { q++; continue; }
            if (*q == '}') break;
            return false;
        }
    }
    if (strcmp(schema, "1") != 0) return false;
    if (strcmp(product, "NovaFile") != 0) return false;
    if (!ver[0] || !url[0] || !sha[0]) return false;
    return true;
}

// updates.cfg: "url=..." and "require_signature=0|1", '#' comments.
bool nova_cfg_parse(const char* text, char* url, size_t un, bool* reqsig) {
    if (!text || !url || !reqsig) return false;
    url[0] = '\0';
    *reqsig = false;
    const char* p = text;
    while (*p) {
        char line[720];
        size_t n = 0;
        while (*p && *p != '\n' && n + 1 < sizeof line) {
            char c = *p++;
            if (c != '\r') line[n++] = c;
        }
        line[n] = '\0';
        if (*p == '\n') p++;
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = '\0';              // trim key AND value tails
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\0') continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        // trim trailing spaces on the key
        char* ke = s + strlen(s);
        while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        if (strcmp(s, "url") == 0 && !url[0]) snprintf(url, un, "%s", val);
        else if (strcmp(s, "require_signature") == 0)
            *reqsig = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
    }
    return true;
}

// ------------------------------------------------------------ versioning
static bool udigits(const char** pp, unsigned* out) {
    const char* p = *pp;
    if (*p < '0' || *p > '9') return false;
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 0xFFFFul) return false;      // absurd segment
        p++;
    }
    *out = (unsigned)v;
    *pp = p;
    return true;
}

int nova_ver_parse(const char* s, unsigned* maj, unsigned* min, unsigned* pat) {
    if (!s || !maj || !min || !pat) return -1;
    unsigned a = 0, b = 0, c = 0;
    const char* p = s;
    if (!udigits(&p, &a) || *p != '.') return -1;
    p++;
    if (!udigits(&p, &b) || *p != '.') return -1;
    p++;
    if (!udigits(&p, &c) || *p != '\0') return -1;
    *maj = a; *min = b; *pat = c;
    return 0;
}

int nova_ver_cmp(const char* a, const char* b) {
    unsigned am = 0, ai = 0, ap = 0, bm = 0, bi = 0, bp = 0;
    if (nova_ver_parse(a, &am, &ai, &ap) != 0) return -2;
    if (nova_ver_parse(b, &bm, &bi, &bp) != 0) return -2;
    if (am != bm) return am < bm ? -1 : 1;
    if (ai != bi) return ai < bi ? -1 : 1;
    if (ap != bp) return ap < bp ? -1 : 1;
    return 0;
}

bool nova_hex64_ok(const char* s) {
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// https everywhere; plain http only for the loopback test rig.
bool nova_url_allowed(const char* url) {
    if (!url || !url[0]) return false;
    if (strncmp(url, "https://", 8) == 0) return true;
    if (strncmp(url, "http://127.0.0.1", 16) == 0) {
        char c = url[16];
        return (c == '\0' || c == '/' || c == ':');
    }
    if (strncmp(url, "http://localhost", 16) == 0) {
        char c = url[16];
        return (c == '\0' || c == '/' || c == ':');
    }
    return false;
}

// ---------------------------------------------------------------- SHA-256
bool nova_sha256_buf(const void* data, size_t n, char out[65]) {
    if (!out) return false;
    out[0] = '\0';
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    bool ok = false;
    UCHAR digest[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                    NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
            if ((n == 0 || BCryptHashData(hash, (PUCHAR)data,
                                           (ULONG)n, 0) == 0) &&
                BCryptFinishHash(hash, digest, 32, 0) == 0) {
                for (int i = 0; i < 32; i++)
                    snprintf(out + i * 2, 3, "%02x", digest[i]);
                out[64] = '\0';
                ok = true;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

bool nova_sha256_file(const char* path, char out[65], long long* size) {
    if (!path || !out) return false;
    out[0] = '\0';
    if (size) *size = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                    NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
            unsigned char* buf = (unsigned char*)malloc(64 * 1024);
            if (buf) {
                long long total = 0;
                bool read_ok = true;
                for (;;) {
                    DWORD got = 0;
                    if (!ReadFile(f, buf, 64 * 1024, &got, NULL)) {
                        read_ok = false;
                        break;
                    }
                    if (got == 0) break;
                    total += got;
                    if (total > UP_PACKAGE_MAX) { read_ok = false; break; }
                    if (BCryptHashData(hash, buf, got, 0) != 0) {
                        read_ok = false;
                        break;
                    }
                }
                UCHAR digest[32];
                if (read_ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
                    for (int i = 0; i < 32; i++)
                        snprintf(out + i * 2, 3, "%02x", digest[i]);
                    out[64] = '\0';
                    if (size) *size = total;
                    ok = true;
                }
                free(buf);
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(f);
    return ok;
}

// ----------------------------------------------------- Authenticode check
// Validates the embedded signature chain (offline-friendly: no revocation
// retrieval) and, when a thumbprint is given, that the SIGNER matches it.
static bool sig_signer_thumb(const char* path, char out[41]) {
    HCERTSTORE store = NULL;
    HCRYPTMSG msg = NULL;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, NULL, NULL, NULL,
                          &store, &msg, NULL))
        return false;
    bool ok = false;
    DWORD sl = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &sl) && sl) {
        PCMSG_SIGNER_INFO si = (PCMSG_SIGNER_INFO)malloc(sl);
        if (si && CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &sl)) {
            CERT_INFO ci;
            memset(&ci, 0, sizeof ci);
            ci.Issuer = si->Issuer;
            ci.SerialNumber = si->SerialNumber;
            PCCERT_CONTEXT cc = CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &ci, NULL);
            if (cc) {
                BYTE hash[20];
                DWORD hn = sizeof hash;
                if (CertGetCertificateContextProperty(cc, CERT_HASH_PROP_ID,
                                                      hash, &hn) && hn == 20) {
                    for (DWORD i = 0; i < 20; i++)
                        snprintf(out + i * 2, 3, "%02x", hash[i]);
                    out[40] = '\0';
                    ok = true;
                }
                CertFreeCertificateContext(cc);
            }
        }
        free(si);
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return ok;
}

bool nova_sig_verify(const char* path, const char* want_thumb_or_null) {
    if (!path || !path[0]) return false;
    WINTRUST_FILE_INFO fi;
    memset(&fi, 0, sizeof fi);
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = NULL;
    {
        wchar_t wpath[MAX_PATH + 4];
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH + 4))
            return false;
        fi.pcwszFilePath = wpath;           // used during the call only
        WINTRUST_DATA wd;
        memset(&wd, 0, sizeof wd);
        wd.cbStruct = sizeof wd;
        wd.dwUIChoice = WTD_UI_NONE;
        wd.fdwRevocationChecks = WTD_REVOKE_NONE;
        wd.dwUnionChoice = WTD_CHOICE_FILE;
        wd.pFile = &fi;
        wd.dwStateAction = WTD_STATEACTION_VERIFY;
        wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        LONG st = WinVerifyTrust(NULL, &action, &wd);
        wd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(NULL, &action, &wd);
        if (st != ERROR_SUCCESS) return false;
    }
    if (!want_thumb_or_null || !want_thumb_or_null[0]) return true;
    char thumb[41];
    if (!sig_signer_thumb(path, thumb)) return false;
    return _stricmp(thumb, want_thumb_or_null) == 0;
}

// -------------------------------------------------------------- WinHTTP
static wchar_t* u2w(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

// GET a small document (the manifest).  Returns malloc'd body or NULL;
// status_out (optional) receives the HTTP status code.
static char* http_get_small(const char* url, long* status_out,
                            char* err, size_t errn) {
    if (status_out) *status_out = 0;
    wchar_t* wurl = u2w(url);
    if (!wurl) { snprintf(err, errn, "url conversion failed"); return NULL; }

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 1024;
    char* body = NULL;
    HINTERNET ses = NULL, con = NULL, req = NULL;

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(err, errn, "bad url");
        goto done;
    }
    {
        wchar_t extra[1024];
        extra[0] = 0;
        if (uc.dwExtraInfoLength && uc.lpszExtraInfo) {
            wcsncpy(extra, uc.lpszExtraInfo, 1023);
            extra[1023] = 0;
        }
        wchar_t full[2048];
        _snwprintf(full, 2047, L"%s%s%s", path, extra[0] ? L"?" : L"", extra);
        full[2047] = 0;

        ses = WinHttpOpen(L"NovaFile",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) { snprintf(err, errn, "winhttp open failed"); goto done; }
        WinHttpSetTimeouts(ses, UP_CONNECT_MS, UP_CONNECT_MS,
                           UP_READ_MS, UP_READ_MS);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
        if (!con) { snprintf(err, errn, "connect setup failed"); goto done; }
        req = WinHttpOpenRequest(con, L"GET", full, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS
                                     ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { snprintf(err, errn, "request setup failed"); goto done; }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            snprintf(err, errn, "request failed (offline?)");
            goto done;
        }
        if (!WinHttpReceiveResponse(req, NULL)) {
            snprintf(err, errn, "no response from server");
            goto done;
        }
        DWORD status = 0, sl = sizeof status;
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
            WINHTTP_NO_HEADER_INDEX);
        if (status_out) *status_out = (long)status;
        if (status != 200) {
            snprintf(err, errn, "server returned HTTP %lu", status);
            goto done;
        }
        size_t cap = 4096, used = 0;
        body = (char*)malloc(cap);
        if (!body) { snprintf(err, errn, "out of memory"); goto done; }
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req, body + used, (DWORD)(cap - used - 1),
                                 &got)) {
                snprintf(err, errn, "read failed");
                free(body); body = NULL;
                goto done;
            }
            if (got == 0) break;
            used += got;
            body[used] = '\0';
            if (used + 1 >= cap) {
                if (cap >= UP_MANIFEST_MAX) {
                    snprintf(err, errn, "manifest too large");
                    free(body); body = NULL;
                    goto done;
                }
                cap *= 2;
                if (cap > UP_MANIFEST_MAX) cap = UP_MANIFEST_MAX;
                char* nb = (char*)realloc(body, cap);
                if (!nb) { free(body); body = NULL;
                           snprintf(err, errn, "out of memory"); goto done; }
                body = nb;
            }
        }
        body[used] = '\0';
    }
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(wurl);
    return body;
}

// Stream the package to disk with progress + hard caps.  Returns false on
// any failure (the partial file is left for diagnosis, never executed).
static bool http_download(const char* url, const char* dest,
                          long long expected, char* err, size_t errn) {
    wchar_t* wurl = u2w(url);
    if (!wurl) { snprintf(err, errn, "url conversion failed"); return false; }
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 1024;
    bool ok = false;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    HANDLE f = INVALID_HANDLE_VALUE;

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(err, errn, "bad url");
        goto done;
    }
    {
        wchar_t extra[1024];
        extra[0] = 0;
        if (uc.dwExtraInfoLength && uc.lpszExtraInfo) {
            wcsncpy(extra, uc.lpszExtraInfo, 1023);
            extra[1023] = 0;
        }
        wchar_t full[2048];
        _snwprintf(full, 2047, L"%s%s%s", path, extra[0] ? L"?" : L"", extra);
        full[2047] = 0;

        ses = WinHttpOpen(L"NovaFile",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) { snprintf(err, errn, "winhttp open failed"); goto done; }
        WinHttpSetTimeouts(ses, UP_CONNECT_MS, UP_CONNECT_MS,
                           UP_READ_MS, UP_READ_MS);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
        if (!con) { snprintf(err, errn, "connect setup failed"); goto done; }
        req = WinHttpOpenRequest(con, L"GET", full, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS
                                     ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { snprintf(err, errn, "request setup failed"); goto done; }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, NULL)) {
            snprintf(err, errn, "download request failed (offline?)");
            goto done;
        }
        DWORD status = 0, sl = sizeof status;
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            snprintf(err, errn, "server returned HTTP %lu", status);
            goto done;
        }
        f = CreateFileA(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            snprintf(err, errn, "cannot write package file");
            goto done;
        }
        unsigned char* buf = (unsigned char*)malloc(64 * 1024);
        if (!buf) { snprintf(err, errn, "out of memory"); goto done; }
        long long got_total = 0;
        bool read_ok = true;
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req, buf, 64 * 1024, &got)) {
                snprintf(err, errn, "download interrupted");
                read_ok = false;
                break;
            }
            if (got == 0) break;
            DWORD wrote = 0;
            if (!WriteFile(f, buf, got, &wrote, NULL) || wrote != got) {
                snprintf(err, errn, "disk write failed");
                read_ok = false;
                break;
            }
            got_total += got;
            if (got_total > UP_PACKAGE_MAX) {
                snprintf(err, errn, "package exceeds the size cap");
                read_ok = false;
                break;
            }
            EnterCriticalSection(&g_up_cs);
            g_up_got = got_total;
            if (expected > 0) g_up_size = expected;
            LeaveCriticalSection(&g_up_cs);
            if (g_up_echo)
                printf("nova-update: downloading %I64d%s bytes\n", got_total,
                       expected > 0 ? "" : " so far");
        }
        free(buf);
        if (!read_ok) goto done;
        if (expected > 0 && got_total != expected) {
            snprintf(err, errn, "size mismatch (got %I64d, want %I64d)",
                     got_total, expected);
            goto done;
        }
        ok = true;
    }
done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(wurl);
    return ok;
}

// ------------------------------------------------------------ the worker
static void up_set_status(const char* s) {
    EnterCriticalSection(&g_up_cs);
    snprintf(g_up_status, sizeof g_up_status, "%s", s);
    LeaveCriticalSection(&g_up_cs);
}

// <appdir>\updates\ready.state - written after a package is fully
// verified, so "Restart and update" (and the --update-apply CLI) still
// work in a later session.  The file is re-hashed before it is executed.
static void up_ready_state_path(char* out, size_t cap) {
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, (DWORD)sizeof base);
    if (n == 0 || n >= sizeof base) { out[0] = '\0'; return; }
    snprintf(out, cap, "%s\\NovaFile\\%s\\ready.state", base, UP_DIR_NAME);
}

static void up_ready_state_write(const char* ver, const char* sha,
                                 const char* pkg) {
    char st[MAX_PATH];
    up_ready_state_path(st, sizeof st);
    if (!st[0]) return;
    FILE* f = fopen(st, "w");
    if (!f) return;
    fprintf(f, "version=%s\nsha256=%s\nfile=%s\n", ver, sha, pkg);
    fclose(f);
}

static bool up_ready_state_load(char* pkg, size_t pcap, char* ver, size_t vcap,
                                char* sha, size_t scap) {
    pkg[0] = ver[0] = sha[0] = '\0';
    char st[MAX_PATH];
    up_ready_state_path(st, sizeof st);
    if (!st[0]) return false;
    HANDLE f = CreateFileA(st, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    char text[1400];
    DWORD got = 0;
    BOOL okread = ReadFile(f, text, sizeof text - 1, &got, NULL);
    CloseHandle(f);
    if (!okread || got == 0) return false;
    text[got] = '\0';
    char* p = text;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = '\0';                  // terminate the line (LF/CRLF)
        if (*p == '\r') p++;                  // tolerate CRLF files
        if (*p == '\n') p++;
        if (line[0] == '#' || line[0] == '\0') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* val = eq + 1;
        if (strcmp(line, "version") == 0) snprintf(ver, vcap, "%s", val);
        else if (strcmp(line, "sha256") == 0) snprintf(sha, scap, "%s", val);
        else if (strcmp(line, "file") == 0) snprintf(pkg, pcap, "%s", val);
    }
    return pkg[0] && ver[0] && sha[0];
}

static void up_fail(const char* why) {
    InterlockedExchange(&g_up_state, UPS_ERROR);
    up_set_status(why);
    if (g_up_echo) printf("nova-update: FAILED: %s\n", why);
}

static void up_work(void) {
    char err[192] = "";
    long status = 0;

    if (!g_up_cfg_url[0]) {
        InterlockedExchange(&g_up_state, UPS_NOT_CONFIGURED);
        up_set_status("No update source is configured - see "
                      "docs/RELEASE.md to set one up.");
        if (g_up_echo) printf("nova-update: not configured\n");
        return;
    }
    if (!nova_url_allowed(g_up_cfg_url)) {
        InterlockedExchange(&g_up_state, UPS_INSECURE);
        up_set_status("Update URL refused: https required.");
        if (g_up_echo) printf("nova-update: insecure url refused\n");
        return;
    }
    up_set_status("Checking for updates...");
    if (g_up_echo) printf("nova-update: checking %s\n", g_up_cfg_url);
    {
        // a stale "ready" pointer must not survive a new check
        char st[MAX_PATH];
        up_ready_state_path(st, sizeof st);
        if (st[0]) DeleteFileA(st);
    }

    char* body = http_get_small(g_up_cfg_url, &status, err, sizeof err);
    if (!body) { up_fail(err[0] ? err : "update check failed"); return; }

    char ver[32], url[640], sha[80], notes[640], minv[32], signer[80];
    long long size = 0;
    bool parsed = nova_manifest_parse(body, ver, sizeof ver, url, sizeof url,
                                      sha, sizeof sha, &size, notes,
                                      sizeof notes, minv, sizeof minv,
                                      signer, sizeof signer);
    free(body);
    if (!parsed) { up_fail("Update manifest rejected (bad format)."); return; }
    {
        unsigned vmaj = 0, vmin = 0, vpat = 0;
        if (nova_ver_parse(ver, &vmaj, &vmin, &vpat) != 0) {
            up_fail("Update manifest has an invalid version.");
            return;
        }
    }
    if (!nova_hex64_ok(sha)) {
        up_fail("Update manifest has an invalid SHA-256.");
        return;
    }
    if (!nova_url_allowed(url)) {
        up_fail("Update package URL refused (https required).");
        return;
    }
    if (size < 0 || size > UP_PACKAGE_MAX) {
        up_fail("Update package size is out of range.");
        return;
    }

    EnterCriticalSection(&g_up_cs);
    snprintf(g_up_ver, sizeof g_up_ver, "%s", ver);
    snprintf(g_up_url, sizeof g_up_url, "%s", url);
    snprintf(g_up_sha, sizeof g_up_sha, "%s", sha);
    snprintf(g_up_notes, sizeof g_up_notes, "%s", notes);
    snprintf(g_up_minv, sizeof g_up_minv, "%s", minv);
    snprintf(g_up_signer, sizeof g_up_signer, "%s", signer);
    g_up_size = size;
    LeaveCriticalSection(&g_up_cs);

    int cmp = nova_ver_cmp(ver, NOVA_VERSION);
    if (cmp == -2) { up_fail("Server sent an unreadable version."); return; }
    if (cmp <= 0) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        InterlockedExchange(&g_up_state, UPS_UPTODATE);
        char s[256];
        snprintf(s, sizeof s,
                 "You're up to date - you already have the latest version "
                 "(%s). Checked %02u:%02u.",
                 NOVA_VERSION, st.wHour, st.wMinute);
        up_set_status(s);
        if (g_up_echo)
            printf("nova-update: up to date (%s >= %s)\n", NOVA_VERSION, ver);
        return;
    }
    // A newer version that no longer supports this build must NOT be
    // auto-installed (its installer could not upgrade this baseline).
    if (minv[0] && nova_ver_cmp(NOVA_VERSION, minv) == -1) {
        InterlockedExchange(&g_up_state, UPS_READY);
        // READY with no local package = "manual action required"
        char s[256];
        snprintf(s, sizeof s,
                 "Update v%s needs NovaFile v%s or newer - download it "
                 "from the website.",
                 ver, minv);
        EnterCriticalSection(&g_up_cs);
        g_up_pkg[0] = '\0';
        LeaveCriticalSection(&g_up_cs);
        up_set_status(s);
        if (g_up_echo) printf("nova-update: %s\n", s);
        return;
    }

    // Package path: <appdir>\updates\NovaFile-Setup-<ver>.exe
    char appdir[MAX_PATH];
    {
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", appdir,
                                          (DWORD)sizeof appdir);
        if (n == 0 || n >= sizeof appdir) {
            up_fail("LOCALAPPDATA missing; cannot store updates.");
            return;
        }
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s\\NovaFile\\%s", appdir, UP_DIR_NAME);
        CreateDirectoryA(dir, NULL);
        snprintf(g_up_pkg, sizeof g_up_pkg, "%s\\NovaFile-Setup-%s.exe",
                 dir, ver);
    }
    char part[MAX_PATH + 8];
    snprintf(part, sizeof part, "%s.part", g_up_pkg);

    // Already downloaded by a previous run? Verify and reuse it.
    {
        char have[80];
        long long have_size = 0;
        if (nova_sha256_file(g_up_pkg, have, &have_size) &&
            _stricmp(have, sha) == 0 &&
            (size == 0 || have_size == size)) {
            if (g_up_echo)
                printf("nova-update: package already present and verified\n");
            goto verified;
        }
    }

    up_set_status("Downloading the update...");
    {
        EnterCriticalSection(&g_up_cs);
        g_up_got = 0;
        LeaveCriticalSection(&g_up_cs);
    }
    if (!http_download(url, part, size, err, sizeof err)) {
        DeleteFileA(part);
        up_fail(err[0] ? err : "download failed");
        return;
    }
    {
        char got[80];
        long long got_size = 0;
        if (!nova_sha256_file(part, got, &got_size)) {
            DeleteFileA(part);
            up_fail("Could not hash the downloaded package.");
            return;
        }
        if (_stricmp(got, sha) != 0) {
            DeleteFileA(part);
            up_fail("SHA-256 mismatch: package discarded.");
            return;
        }
        if (!MoveFileExA(part, g_up_pkg, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileA(part);
            up_fail("Could not finalise the package file.");
            return;
        }
    }

verified:
    // Publisher identity: a manifest "signer" thumbprint makes the
    // signature mandatory and pins it; require_signature=1 demands any
    // valid signature.  Without either, the https transport + SHA-256 are
    // the verification (documented in docs/RELEASE.md).
    {
        char want[80];
        EnterCriticalSection(&g_up_cs);
        snprintf(want, sizeof want, "%s", g_up_signer);
        LeaveCriticalSection(&g_up_cs);
        if (want[0]) {
            if (!nova_sig_verify(g_up_pkg, want)) {
                DeleteFileA(g_up_pkg);
                up_fail("Signature missing or signer mismatch: discarded.");
                return;
            }
        } else if (g_up_cfg_reqsig) {
            if (!nova_sig_verify(g_up_pkg, NULL)) {
                DeleteFileA(g_up_pkg);
                up_fail("Package not Authenticode-signed (required).");
                return;
            }
        }
    }
    InterlockedExchange(&g_up_state, UPS_READY);
    up_ready_state_write(ver, sha, g_up_pkg);   // apply may come later
    {
        char s[256];
        snprintf(s, sizeof s,
                 "Update v%s downloaded and verified - click Update Now "
                 "to install it.", ver);
        up_set_status(s);
    }
    if (g_up_echo) printf("nova-update: ready (v%s)\n", ver);
}

static DWORD WINAPI up_worker(LPVOID arg) {
    (void)arg;
    up_work();
    return 0;
}

// --------------------------------------------------------- public module
bool update_init(void) {
    if (!g_up_cs_ready) {
        InitializeCriticalSection(&g_up_cs);
        g_up_cs_ready = true;
    }
    snprintf(g_up_show, sizeof g_up_show,
             "Not checked yet - click Check for updates.");
    snprintf(g_up_status, sizeof g_up_status,
             "Not checked yet - click Check for updates.");
    if (!g_up_cfg_read) {
        g_up_cfg_read = true;
        char appdir[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", appdir,
                                          (DWORD)sizeof appdir);
        if (n > 0 && n < sizeof appdir) {
            char path[MAX_PATH + 40];
            snprintf(path, sizeof path, "%s\\NovaFile\\%s", appdir,
                     UP_CFG_NAME);
            HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                char text[1600];
                DWORD got = 0;
                if (ReadFile(f, text, sizeof text - 1, &got, NULL) && got > 0) {
                    text[got] = '\0';
                    nova_cfg_parse(text, g_up_cfg_url, sizeof g_up_cfg_url,
                                   &g_up_cfg_reqsig);
                }
                CloseHandle(f);
            }
        }
        // Test/ops override always wins over the config file.
        char env[640];
        if (GetEnvironmentVariableA("NOVA_UPDATE_URL", env,
                                    (DWORD)sizeof env) > 0)
            snprintf(g_up_cfg_url, sizeof g_up_cfg_url, "%s", env);
    }
    return true;
}

void update_shutdown(void) {
    if (g_up_thread) {
        WaitForSingleObject(g_up_thread, 5000);
        CloseHandle(g_up_thread);
        g_up_thread = NULL;
    }
}

void update_startup(void) {
    char env[8];
    if (GetEnvironmentVariableA("NOVA_UPDATE_DISABLE", env,
                                (DWORD)sizeof env) > 0 &&
        env[0] == '1')
        return;                              // hard kill-switch
    if (GetEnvironmentVariableA("NOVA_OFFSCREEN", env, (DWORD)sizeof env) > 0)
        return;                              // headless test runs stay offline
    update_kick_check(false);
}

void update_kick_check(bool manual) {
    (void)manual;
    // one worker at a time; a finished check (terminal state) may be
    // re-run on demand - that is what the Settings button does.
    if (InterlockedCompareExchange(&g_up_state, UPS_BUSY, UPS_BUSY) ==
        UPS_BUSY)
        return;
    InterlockedExchange(&g_up_state, UPS_BUSY);
    update_init();                           // idempotent: config once
    if (g_up_thread) {
        WaitForSingleObject(g_up_thread, 1000);
        CloseHandle(g_up_thread);
        g_up_thread = NULL;
    }
    g_up_thread = CreateThread(NULL, 0, up_worker, NULL, 0, NULL);
    if (!g_up_thread) {
        InterlockedExchange(&g_up_state, UPS_IDLE);
        up_set_status("Could not start the update worker.");
        return;
    }
}

void update_pump(void) {
    if (!g_up_cs_ready) return;
    EnterCriticalSection(&g_up_cs);
    snprintf(g_up_show, sizeof g_up_show, "%s", g_up_status);
    LeaveCriticalSection(&g_up_cs);
}

const char* update_status_text(void) { return g_up_show; }

const char* update_source_text(void) {
    return g_up_cfg_url[0] ? g_up_cfg_url : "Not configured";
}

bool update_ready(void) {
    return InterlockedCompareExchange(&g_up_state, UPS_IDLE, UPS_IDLE) ==
               UPS_READY && g_up_pkg[0];
}

bool update_apply(void) {
    char pkg[MAX_PATH];
    EnterCriticalSection(&g_up_cs);
    snprintf(pkg, sizeof pkg, "%s", g_up_pkg);
    LeaveCriticalSection(&g_up_cs);
    if (!pkg[0]) {
        // A later session (or the --update-apply CLI): recover the
        // package from ready.state - but only after re-hashing it and
        // confirming it really is a NEWER version (no stale re-apply,
        // no tampering between verify and exec).
        char ver[32], sha[80], have[80];
        long long have_size = 0;
        if (up_ready_state_load(pkg, sizeof pkg, ver, sizeof ver,
                                sha, sizeof sha) &&
            nova_ver_cmp(ver, NOVA_VERSION) == 1 &&
            nova_sha256_file(pkg, have, &have_size) &&
            _stricmp(have, sha) == 0) {
            EnterCriticalSection(&g_up_cs);
            snprintf(g_up_pkg, sizeof g_up_pkg, "%s", pkg);
            LeaveCriticalSection(&g_up_cs);
        } else {
            pkg[0] = '\0';
        }
    }
    if (!pkg[0]) {
        up_set_status("No verified update is downloaded yet.");
        return false;
    }
    char cmdline[MAX_PATH + 48];
    snprintf(cmdline, sizeof cmdline,
             "\"%s\" /SILENT /SUPPRESSMSGBOXES /UPDATE", pkg);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL,
                        &si, &pi)) {
        up_set_status("Could not launch the installer.");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    up_set_status("Installing update... the app will reopen itself.");
    return true;                             // caller: quit the app now
}

bool update_open_notes(void) {
    char notes[640];
    EnterCriticalSection(&g_up_cs);
    snprintf(notes, sizeof notes, "%s", g_up_notes);
    LeaveCriticalSection(&g_up_cs);
    if (!notes[0] || !nova_url_allowed(notes)) return false;
    HINSTANCE h = ShellExecuteA(NULL, "open", notes, NULL, NULL,
                                SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
}

// ------------------------------------------------------------------ CLI
int update_cli(int mode) {
    update_init();
    if (mode == 2) {                         // --update-apply
        if (!update_apply()) {
            printf("nova-update: status: %s\n", g_up_status);
            printf("RESULT: APPLY_FAILED\n");
            return 1;
        }
        printf("nova-update: installer launched\n");
        printf("RESULT: APPLYING\n");
        return 0;
    }
    g_up_echo = true;
    printf("nova-update: installed version %s\n", NOVA_VERSION);
    up_work();
    LONG s = InterlockedCompareExchange(&g_up_state, UPS_IDLE, UPS_IDLE);
    printf("nova-update: status: %s\n", g_up_status);
    printf("RESULT: %s\n",
           s == UPS_READY          ? (g_up_pkg[0] ? "UPDATE_READY"
                                                  : "MANUAL_REQUIRED")
         : s == UPS_UPTODATE       ? "UP_TO_DATE"
         : s == UPS_NOT_CONFIGURED ? "NOT_CONFIGURED"
         : s == UPS_INSECURE       ? "INSECURE_URL"
         :                           "ERROR");
    switch (s) {
    case UPS_READY:          return g_up_pkg[0] ? 2 : 1;
    case UPS_UPTODATE:       return 0;
    case UPS_NOT_CONFIGURED: return 3;
    case UPS_INSECURE:       return 4;
    default:                 return 1;
    }
}
