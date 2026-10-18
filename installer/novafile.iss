; ==============================================================
; NovaFile Explorer - Inno Setup 6 installer (per-user, no admin)
;
; Design (matches the app's architecture):
;   * Per-user install: PrivilegesRequired=lowest, target
;     {localappdata}\Programs\NovaFile - no UAC prompt, no admin.
;   * User data lives in {localappdata}\NovaFile (settings.txt,
;     favorites, thumbnails, updates.cfg) - OUTSIDE {app}, so an
;     upgrade never overwrites it and an uninstall never deletes it.
;   * Same AppId across releases = upgrade-in-place: the new setup
;     replaces files in the same directory and refreshes the
;     Add/Remove entry.  The previous exe is backed up as
;     NovaFile.exe.prev and only dropped on the next install.
;   * Silent mode (/SILENT) is the auto-update path: the updater
;     downloads the package, verifies SHA-256 (+ Authenticode when
;     configured), then launches it with /SILENT /UPDATE.  After the
;     files are replaced, the installed build must pass its own
;     --selftest; on failure the backup is restored and setup exits
;     with an error - the user keeps a working app.
;   * /UPDATE relaunches NovaFile after a successful update.
;     NOVA_UPDATE_NORELAUNCH=1 suppresses the relaunch (test rig).
;   * Uninstall leaves user data alone; uninstall.exe /DELETESETTINGS
;     removes {localappdata}\NovaFile explicitly.
;
; Build:  installer\build_installer.bat   (reads include\version.h)
; ==============================================================

#ifndef AppVer
#define AppVer "0.0.0-dev"
#endif
#ifndef AppVerNum
#define AppVerNum "0.0.0.0"
#endif
#ifndef Publisher
#define Publisher "NovaFile Project"
#endif
#define MyAppName "NovaFile Explorer"

[Setup]
AppId={{7F3A9C21-5D84-4E6B-9C07-2A81F4D6B3E5}
AppName=NovaFile Explorer
AppVersion={#AppVer}
AppPublisher={#Publisher}
VersionInfoVersion={#AppVerNum}
VersionInfoProductTextVersion={#AppVer}
DefaultDirName={localappdata}\Programs\NovaFile
DefaultGroupName=NovaFile Explorer
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=NovaFile-Setup-{#AppVer}
SetupIconFile=..\NovaFile.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\NovaFile.exe
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
MinVersion=6.1sp1
ChangesAssociations=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\NovaFile.exe"; DestDir: "{app}"; \
    Flags: ignoreversion

[Icons]
Name: "{autoprograms}\NovaFile Explorer"; Filename: "{app}\NovaFile.exe"
Name: "{autodesktop}\NovaFile Explorer"; Filename: "{app}\NovaFile.exe"; \
    Tasks: desktopicon

[Run]
Filename: "{app}\NovaFile.exe"; \
    Description: "Launch {#MyAppName}"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; {app} only - user data in {localappdata}\NovaFile is never touched here
Type: filesandordirs; Name: "{app}"

[Code]
function CmdLineParamExists(const Value: string): Boolean;
var I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Value) = 0 then begin
      Result := True;
      Exit;
    end;
end;

// Upgrade safety: keep the build we are about to replace until the new
// one proves itself (silent post-install selftest below).
procedure CurStepChanged(CurStep: TSetupStep);
var ResultCode: Integer;
    AppExe, Prev: String;
begin
  AppExe := ExpandConstant('{app}\NovaFile.exe');
  Prev := AppExe + '.prev';
  if CurStep = ssInstall then begin
    if FileExists(AppExe) then
      CopyFile(AppExe, Prev, False);
  end;
  if CurStep = ssPostInstall then begin
    // Silent installs are the update path: the freshly installed build
    // must pass its own headless selftest before it is trusted.
    if WizardSilent then begin
      if Exec(AppExe, '--selftest', '', SW_HIDE,
              ewWaitUntilTerminated, ResultCode) then begin
        if ResultCode <> 0 then begin
          if FileExists(Prev) then begin
            DeleteFile(AppExe);
            RenameFile(Prev, AppExe);
          end;
          RaiseException('NovaFile failed its post-install selftest (exit code '
              + IntToStr(ResultCode) + '). The previous version was restored.');
        end;
      end else begin
        RaiseException('NovaFile could not be started to verify the installation.');
      end;
    end;
    // Update flow relaunches the app; the test rig can suppress it.
    if CmdLineParamExists('/UPDATE') and
       (ExpandConstant('{env:NOVA_UPDATE_NORELAUNCH}') <> '1') then
      Exec(AppExe, '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
  end;
end;

// Opt-in data cleanup: uninstall.exe /DELETESETTINGS
function InitializeUninstall(): Boolean;
begin
  Result := True;
  if CmdLineParamExists('/DELETESETTINGS') then
    DelTree(ExpandConstant('{localappdata}\NovaFile'), True, True, True);
end;
