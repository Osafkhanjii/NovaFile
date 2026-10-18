// version.h - single source of truth for the NovaFile release version.
// Consumed by:
//   src/app/update.c        - update checks + Settings > Updates display
//   resource.rc             - VERSIONINFO block (preprocessed by windres)
//   installer\build_installer.bat - passes /DAppVer /DAppVerComma to ISCC
//   tools\release\make_release.ps1 - manifest generation + consistency check
#ifndef NOVA_VERSION_H
#define NOVA_VERSION_H

#define NOVA_VERSION_MAJOR 1
#define NOVA_VERSION_MINOR 0
#define NOVA_VERSION_PATCH 0
#define NOVA_VERSION       "1.0.0"

#define NOVA_PRODUCT    "NovaFile Explorer"
// Publisher shown in the installer and the VERSIONINFO resource - set this
// to your name or company before publishing a release.
#define NOVA_PUBLISHER  "NovaFile Project"

#endif // NOVA_VERSION_H
