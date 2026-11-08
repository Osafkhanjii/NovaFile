// version.h - single source of truth for the NovaFile release version.
// Consumed by:
//   src/app/update.c        - update checks + Settings > Updates display
//   resource.rc             - VERSIONINFO block (preprocessed by windres)
//   installer\build_installer.bat - passes /DAppVer /DAppVerComma to ISCC
//   tools\release\make_release.ps1 - manifest generation + consistency check
#ifndef NOVA_VERSION_H
#define NOVA_VERSION_H

#define NOVA_VERSION_MAJOR 1
#define NOVA_VERSION_MINOR 2
#define NOVA_VERSION_PATCH 1
#define NOVA_VERSION       "1.2.1"

#define NOVA_PRODUCT    "NovaFile Explorer"
// Publisher shown in the installer and the VERSIONINFO resource - set this
// to your name or company before publishing a release.
#define NOVA_PUBLISHER  "NovaFile Project"

// Where installed copies look for new releases when the user has no
// %LOCALAPPDATA%\NovaFile\updates.cfg of their own.  GitHub serves the
// newest release's latest.json under this fixed address (it redirects to
// the asset), so every published release is found automatically.
#define NOVA_UPDATE_DEFAULT_URL \
    "https://github.com/Osafkhanjii/NovaFile/releases/latest/download/latest.json"

#endif // NOVA_VERSION_H
