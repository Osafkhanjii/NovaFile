# NovaFile Explorer — Releases, Hosting & Auto-Updates

This is the complete picture of how a NovaFile release is built, hosted,
signed, and delivered to installed copies. Everything is designed around
the existing app: one C executable, no runtime dependencies, per-user
install, user data outside the install directory.

---

## 1. The moving parts

| Piece | Where | What it does |
|---|---|---|
| Version | `include/version.h` | Single source of truth (`NOVA_VERSION`). Used by the exe's VERSIONINFO, the installer, the update client and the release script. |
| Update client | `src/app/update.c` | In-app updater: manifest check, background download, SHA-256 + size verification, optional Authenticode check, silent installer launch. Uses only Windows system DLLs (winhttp/bcrypt/crypt32/wintrust). |
| Installer | `installer/novafile.iss` | Inno Setup 6, **per-user** (no admin/UAC), Start Menu + optional desktop icon, ARP uninstall entry, upgrade-in-place with `.prev` rollback, post-install selftest in silent mode. |
| Release script | `tools/release/make_release.ps1` | Build → test → installer → hash → (sign) → manifest → verify → stage/publish. |
| Config | `tools/release/release.config.json` | Your hosting URL, publish folder, signing thumbprint. **Not committed** (see sample). |
| On your website | see §4 | `latest.json` + installer + release notes, over HTTPS. |

End-user data lives in `%LOCALAPPDATA%\NovaFile` (settings.txt,
favorites, thumbnails, `updates.cfg`, downloaded packages) — **never**
inside the install dir, so installs/uninstalls/updates never touch it.

---

## 2. One-time setup on your machine

1. **Inno Setup 6** (build-time only, the app itself needs nothing):
   <https://jrsoftware.org/isdl.php> — silent per-user install:
   `innosetup-6.x.x.exe /VERYSILENT /CURRENTUSER`
   The build finds it in `%LOCALAPPDATA%\Programs\InnoSetup6` or
   `C:\Program Files (x86)\Inno Setup 6`, or via `%ISCC%`.
2. **Release config** (keeps URLs out of the repo):
   copy `tools\release\release.config.sample.json` to
   `tools\release\release.config.json` and fill in:

   ```json
   {
     "base_url": "https://YOUR-HOST.example/novafile/",
     "publish_dir": "C:\\deploy\\novafile",
     "sign_thumbprint": "",
     "min_version": "1.0.0"
   }
   ```

   `base_url` must be `https://` (loopback `http://127.0.0.1` is allowed
   only for local test rigs). `publish_dir` is a local folder your
   FTP/SFTP/rsync client syncs — credentials stay in *that* tool, never
   in this repo. Leave it empty to just stage artifacts in `dist\release`.
3. **Signing certificate** (strongly recommended before public
   releases): any code-signing cert (EV recommended for instant
   SmartScreen reputation). Put its thumbprint in `sign_thumbprint`.
   Test-signing with a self-signed cert works for the *mechanism* but
   users must import the cert — fine for a closed group, not for the
   public. Keys/cert files stay in your Windows cert store / HSM, never
   in the repo.
4. **Point installed copies at your server** — write
   `%LOCALAPPDATA%\NovaFile\updates.cfg`:

   ```
   url=https://YOUR-HOST.example/novafile/latest.json
   require_signature=1
   ```

   `require_signature=1` demands a valid Authenticode signature on every
   downloaded package (set it once you sign releases; leave 0/absent
   until then — transport is still HTTPS + SHA-256 pinned in the
   manifest). A new install ships with no `updates.cfg` → the Updates
   section shows "Not configured" and no automatic checks run. The
   `NOVA_UPDATE_URL` environment variable overrides the URL (test rigs).

---

## 3. Cutting a release

1. Bump the version in `include/version.h` (three numbers + string).
2. Update your changelog/notes (the generated notes page has a placeholder).
3. Run:

   ```
   powershell -ExecutionPolicy Bypass -File tools\release\make_release.ps1
   ```

   The script: builds the exe (fails on any warning-free-but-error build),
   runs the full selftest (must be FAIL-free) + a 10 s headless boot
   smoke test, builds the installer, hashes it, signs it if configured,
   writes `latest.json` + notes + `SHA256SUMS.txt` into `dist\release\`,
   then **re-hashes the staged copy and cross-checks the manifest** so a
   corrupted staging step can never ship. Finally it copies everything to
   `publish_dir` (or prints the upload instructions).
4. Upload/sync `dist\release\*` to your host keeping the same relative
   paths. Existing installs pick the change up on their next check —
   nothing else to do.

Switches: `-SkipTests` (packaging only — never publish such a build),
`-NoPublish` (stage but don't copy), `-ConfigPath other.json`.

### Website layout

```
https://YOUR-HOST.example/novafile/
├── latest.json                 <- what clients poll (small, cache-bust it)
├── NovaFile-Setup-1.2.0.exe    <- keep older installers if you like
├── NovaFile-Setup-1.1.0.exe
└── notes/
    └── 1.2.0.html              <- release notes opened from Settings
```

`latest.json` example (generated by the script):

```json
{
  "schema": "1",
  "product": "NovaFile",
  "version": "1.2.0",
  "url": "https://YOUR-HOST.example/novafile/NovaFile-Setup-1.2.0.exe",
  "sha256": "…64 hex…",
  "size": 2712345,
  "notes": "https://YOUR-HOST.example/novafile/notes/1.2.0.html",
  "min_version": "1.0.0"
}
```

Serve `latest.json` with a short cache lifetime or a cache-busting query
(e.g. `latest.json?t=YYYYMMDD` in `updates.cfg`) so CDNs don't hold an
old manifest. Keep **all** older setup EXEs online: a client on any old
version must still be able to reach the current one (there is no
delta/differential update — packages are small, full downloads are the
reliable choice).

---

## 4. What the client actually does (trust model)

On startup (Automatic ON) or via **Settings → Updates → Check for
updates**, the app:

1. Fetches the manifest URL over HTTPS (certificates validated; plain
   HTTP refused except loopback test rigs).
2. Validates the manifest strictly: `schema=1`, `product=NovaFile`, a
   `X.Y.Z` version, a `https://` package URL, a 64-hex `sha256`.
3. Compares versions numerically. Same or older = "Up to date" —
   **downgrades are never applied**; `min_version` above the installed
   version means "manual download required", not an auto-install.
4. Downloads to `%LOCALAPPDATA%\NovaFile\updates\*.exe.part` in the
   background (UI stays responsive), enforcing `size` and a hard cap.
5. Verifies SHA-256 **and** size; then, if the manifest carried a
   `signer` thumbprint or `require_signature=1` is set, verifies the
   Authenticode signature (and that the signer matches the pinned
   thumbprint). Any failure deletes the file.
6. Writes `updates\ready.state` (version + hash + path). "Restart and
   update" — or `--update-apply` in a later session — **re-hashes the
   file again** before executing it, so a package tampered between
   verify and apply is refused.
7. Launches the installer silently (`/SILENT /SUPPRESSMSGBOXES
   /UPDATE`). The installer backs up the current exe as
   `NovaFile.exe.prev`, replaces files, runs the new build's own
   `--selftest`; on failure it restores the backup and exits non-zero.
   On success (and `/UPDATE`) it relaunches the app. The updater never
   overwrites the running exe itself and never runs a package that
   failed verification.

What the manifest **cannot** do: it only ever names a version, a URL and
a hash — it cannot execute commands, and (with signing on) only your key
can authorize a package that clients will run.

Residual caveats (documented, not hidden): a process running as the same
user could edit `updates.cfg`/`ready.state` locally — local-admin/user
compromise is out of scope (it could edit the exe directly anyway);
until you configure signing, package authenticity rests on HTTPS +
hash-pin; unsigned installers trigger SmartScreen "More info → Run
anyway" on first download.

---

## 5. Manual/test paths

| Want | How |
|---|---|
| Check once from a shell | `NovaFile.exe --update-check` (exit 0 up-to-date, 2 ready, 3 not configured, 4 insecure URL, 1 error; prints `RESULT:` line) |
| Apply a downloaded update silently | `NovaFile.exe --update-apply` |
| Point a build at a rig | `set NOVA_UPDATE_URL=http://127.0.0.1:8942/latest.json` |
| Suppress app auto-check entirely | `NOVA_UPDATE_DISABLE=1` |
| Local test rig | `python -m http.server 8942` in a folder with `latest.json` + installer; loopback HTTP is the only non-HTTPS the client accepts |
| Remove user data on uninstall | `unins000.exe /SILENT /DELETESETTINGS` (plain uninstall keeps data) |

The in-app **Settings → Updates** section shows: update source, current
version, the automatic-check switch (default ON, persisted like every
other setting), live status (checking / up to date / vX available /
downloading % / ready / failure reason), and the two buttons **Check for
updates** and **Restart and update** plus **Release notes**.

---

## 6. Tested (background/automated) vs pending

Verified by the automated rig on this machine (2026-10-15): clean
install; upgrade over existing install (backup `.prev`); uninstall keeps
user data and removes ARP entry/shortcuts; full update flow 1.0.0→1.0.1
(check → download → hash-verify → silent apply → new version live,
settings byte-identical); refusal of corrupt SHA, wrong product, bad
schema, malformed JSON, insecure URLs, offline server, same-version,
downgrade, forged `ready.state`, tampered package, missing package;
idempotent re-download skip; `--update-check`/`--update-apply` exit
codes; VERSIONINFO (name/publisher/version) present.

Pending manual checks (need a real desktop session): the visible
restart-to-update relaunch (suppressed in the background rig via
`NOVA_UPDATE_NORELAUNCH=1`), the Settings → Updates screens' look and
feel, a download over a real slow internet connection, SmartScreen
behavior of your signed vs unsigned installer, and an actual upload to
your host.
