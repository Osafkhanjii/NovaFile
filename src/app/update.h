// update.h - NovaFile auto-update client (pure C, Windows system DLLs only:
// winhttp / bcrypt / crypt32 / wintrust - no third-party dependencies).
//
// Model:
//   - update_init() reads %LOCALAPPDATA%\NovaFile\updates.cfg (url=,
//     require_signature=0|1) plus the NOVA_UPDATE_URL test override.
//   - update_kick_check() spawns ONE detached worker that checks the
//     manifest, and - when a strictly newer version exists - downloads the
//     package in the background, verifies size + SHA-256 (+ Authenticode
//     when a signer is configured), and parks it for update_apply().
//   - update_pump() is polled once per frame (house style: no window
//     messages between threads) and refreshes the Settings status text.
//   - update_apply() launches the verified installer silently and asks the
//     app to quit; the installer relaunches the new build.
//
// Security:
//   - Only https:// URLs are accepted; plain http is accepted solely for
//     http://127.0.0.1 and http://localhost so the offline test rig can
//     run a local server (documented in docs/RELEASE.md).
//   - Downgrades are refused (remote <= installed = "up to date").
//   - Packages must match the manifest SHA-256 and size before they are
//     ever executed; a manifest "signer" thumbprint additionally requires
//     a matching Authenticode signature.
#ifndef NOVA_UPDATE_H
#define NOVA_UPDATE_H

#include <stdbool.h>

// ---- lifecycle (called from main.c) ----
bool update_init(void);                     // read config; no network
void update_shutdown(void);                 // stop the worker thread
void update_startup(void);                  // auto-check gate (GUI sessions)
void update_pump(void);                     // per-frame poll + status publish

// ---- actions ----
void update_kick_check(bool manual);        // async manifest check (+download)
bool update_apply(void);                    // run verified installer /SILENT
bool update_open_notes(void);               // ShellExecute the notes URL

// ---- settings integration ----
const char* update_status_text(void);       // dynamic row text (stable ptr)
const char* update_source_text(void);       // configured URL or "Not configured"
bool update_ready(void);                    // verified package waiting

// ---- pure logic (exercised by --selftest) ----
int  nova_ver_parse(const char* s, unsigned* maj, unsigned* min, unsigned* pat);
int  nova_ver_cmp(const char* a, const char* b);   // -2 = invalid input
bool nova_hex64_ok(const char* s);
bool nova_url_allowed(const char* url);
bool nova_manifest_parse(const char* json,
                         char* ver, size_t vn, char* url, size_t un,
                         char* sha, size_t sn, long long* size,
                         char* notes, size_t non, char* minv, size_t mn,
                         char* signer, size_t sgn);
bool nova_cfg_parse(const char* text, char* url, size_t un, bool* reqsig);
bool nova_sha256_buf(const void* data, size_t n, char out[65]);
bool nova_sha256_file(const char* path, char out[65], long long* size);
bool nova_sig_verify(const char* path, const char* want_thumb_or_null);

// ---- CLI (main.c --update-check / --update-apply) ----
// mode 1 = check (+download+verify), 2 = apply. Returns the process exit
// code: 0 up-to-date, 2 update ready, 3 not configured, 4 insecure URL,
// 1 error/offline/verification failure.
int update_cli(int mode);

#endif // NOVA_UPDATE_H
