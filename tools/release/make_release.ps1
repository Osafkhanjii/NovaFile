# ==============================================================
#  make_release.ps1 - reproducible NovaFile Explorer release build
#
#  Steps: 1 version  2 build  3 tests  4 installer  5 hash
#         6 optional Authenticode sign  7 manifest + notes
#         8 self-verify  9 stage/publish
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File tools\release\make_release.ps1
#    ... -SkipTests        (build+package only; NOT for publishing)
#    ... -NoPublish        (stage artifacts, never copy anywhere)
#    ... -ConfigPath my.json
#
#  Config (release.config.json next to this script, or -ConfigPath):
#    {
#      "base_url":     ""    <- REQUIRED for updates, e.g.
#                               "https://your-host.example/novafile/"
#                               (HTTPS only; loopback http for local rigs)
#      "publish_dir":  ""    <- local folder to copy artifacts into
#                               (map to your hosting sync/FTP client)
#      "sign_thumbprint": "" <- code-signing cert thumbprint (optional;
#                               empty = publish UNSIGNED, script warns)
#      "min_version":  "0.0.0"
#    }
#  Keys and tokens stay OUT of the repo: keep the real config file out of
#  version control (release.config.sample.json is the committed template).
# ==============================================================
param(
    [string]$ConfigPath = "",
    [switch]$SkipTests,
    [switch]$NoPublish
)
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir '..\..')
function Fail([string]$msg) { Write-Host "RELEASE FAILED: $msg" -ForegroundColor Red; exit 1 }

# ---- 0. config --------------------------------------------------------
if ($ConfigPath -eq '') { $ConfigPath = Join-Path $ScriptDir 'release.config.json' }
$Cfg = @{ base_url = ''; publish_dir = ''; sign_thumbprint = ''; min_version = '0.0.0' }
if (Test-Path $ConfigPath) {
    $j = Get-Content $ConfigPath -Raw | ConvertFrom-Json
    foreach ($k in @('base_url', 'publish_dir', 'sign_thumbprint', 'min_version')) {
        if ($null -ne $j.$k -and "$($j.$k)" -ne '') { $Cfg[$k] = "$($j.$k)" }
    }
    Write-Host "config: $ConfigPath"
} else {
    Write-Host "config: none found - defaults (unsigned staging only)"
}
$BaseUrl = $Cfg['base_url']
if ($BaseUrl -ne '' -and $BaseUrl -notmatch '^https://') {
    if ($BaseUrl -notmatch '^http://127\.0\.0\.1(:|/)') {
        Fail "base_url must be https:// (got '$BaseUrl')"
    }
}
if ($BaseUrl -ne '' -and $BaseUrl -notmatch '/$') { $BaseUrl = "$BaseUrl/" }

# ---- 1. version (single source: include\version.h) --------------------
$vpath = Join-Path $Root 'include\version.h'
$vtext = Get-Content $vpath -Raw
function Grab([string]$re) {
    if ($vtext -match $re) { return $Matches[1] }
    Fail "include\version.h: pattern not found ($re)"
}
$Major = [int](Grab '#define\s+NOVA_VERSION_MAJOR\s+(\d+)')
$Minor = [int](Grab '#define\s+NOVA_VERSION_MINOR\s+(\d+)')
$Patch = [int](Grab '#define\s+NOVA_VERSION_PATCH\s+(\d+)')
$Ver   = Grab '#define\s+NOVA_VERSION\s+"([^"]+)"'
$Publisher = Grab '#define\s+NOVA_PUBLISHER\s+"([^"]+)"'
Write-Host "=== NovaFile Explorer release $Ver ($Major.$Minor.$Patch.0) ==="

# ---- 2. build ---------------------------------------------------------
Write-Host '[1/9] build (compile.bat)...'
$bc = cmd /c "echo.|`"$Root\compile.bat`"" 2>&1
if ($LASTEXITCODE -ne 0) { $bc | Write-Host; Fail 'compile.bat failed' }
if ($bc -match 'BUILD FAILED') { $bc | Write-Host; Fail 'compile.bat reported failure' }
$Exe = Join-Path $Root 'build\NovaFile.exe'
if (-not (Test-Path $Exe)) { Fail 'build\NovaFile.exe missing' }
$fvi = (Get-Item $Exe).VersionInfo
if ($fvi.FileVersion -ne $Ver) {
    Fail "exe FileVersion '$($fvi.FileVersion)' != version.h '$Ver' (stale build?)"
}

# ---- 3. tests ---------------------------------------------------------
if (-not $SkipTests) {
    Write-Host '[2/9] selftest...'
    $env:NOVA_OFFSCREEN = '1'
    $st = & $Exe --selftest 2>&1 | Out-String
    $rc = $LASTEXITCODE
    Remove-Item Env:NOVA_OFFSCREEN -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Write-Host $st; Fail "selftest exit=$rc" }
    if ($st -match 'FAIL') { Write-Host $st; Fail 'selftest reported FAIL' }
    if ($st -notmatch 'updates:\s+\d+/\d+\s+check\(s\)\s+ok') {
        Write-Host $st; Fail 'selftest updates section missing'
    }
    Write-Host '    selftest OK (no FAIL, updates section present)'

    Write-Host '[3/9] boot smoke (10 s headless run)...'
    $bootlog = Join-Path $env:TEMP 'nova_release_boot.log'
    $env:NOVA_OFFSCREEN = '1'
    $p = Start-Process -FilePath $Exe -RedirectStandardOutput $bootlog -RedirectStandardError "$bootlog.err" -PassThru
    Start-Sleep -Seconds 10
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:NOVA_OFFSCREEN -ErrorAction SilentlyContinue
    $blog = ''
    if (Test-Path $bootlog) { $blog = Get-Content $bootlog -Raw }
    $oks = ([regex]::Matches($blog, '\[OK\]')).Count
    $fails = ([regex]::Matches($blog, 'FAIL')).Count
    if ($oks -lt 10 -or $fails -gt 0) { Fail "boot smoke bad: [OK]=$oks FAIL=$fails" }
    Write-Host "    boot OK ($oks x [OK], 0 FAIL)"
} else {
    Write-Host '[2-3/9] tests SKIPPED (-SkipTests) - do not publish this build'
}

# ---- 4. installer -----------------------------------------------------
Write-Host '[4/9] installer (Inno Setup)...'
$ib = cmd /c "`"$Root\installer\build_installer.bat`"" 2>&1
if ($LASTEXITCODE -ne 0) { $ib | Write-Host; Fail 'installer build failed' }
$Setup = Join-Path $Root "dist\NovaFile-Setup-$Ver.exe"
if (-not (Test-Path $Setup)) { Fail "missing $Setup" }

# ---- 5. hash ----------------------------------------------------------
Write-Host '[5/9] SHA-256...'
$Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Setup).Hash.ToLower()
$Size = (Get-Item -LiteralPath $Setup).Length
Write-Host "    $Hash  ($Size bytes)"

# ---- 6. optional signing ----------------------------------------------
$Signed = $false
if ($Cfg['sign_thumbprint'] -ne '') {
    Write-Host "[6/9] signing with thumbprint $($Cfg['sign_thumbprint'])..."
    $signtool = $null
    foreach ($cand in @("$env:ProgramFiles(x86)\Windows Kits\10\bin\*\x64\signtool.exe",
                        "$env:ProgramFiles\Windows Kits\10\bin\*\x64\signtool.exe")) {
        $found = Get-Item $cand -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
        if ($found) { $signtool = $found.FullName; break }
    }
    if (-not $signtool) { Fail 'sign_thumbprint set but signtool.exe not found (Windows SDK)' }
    & $signtool sign /sha1 $Cfg['sign_thumbprint'] /fd SHA256 /td SHA256 /tr 'http://timestamp.digicert.com' $Setup
    if ($LASTEXITCODE -ne 0) { Fail 'signtool failed' }
    $Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Setup).Hash.ToLower()
    $Signed = $true
    Write-Host "    signed. new hash: $Hash"
} else {
    Write-Host '[6/9] SIGNING SKIPPED - artifacts are UNSIGNED (SmartScreen warning for users).'
    Write-Host '      Set sign_thumbprint in release.config.json before publishing publicly.'
}

# ---- 7. manifest + release notes --------------------------------------
Write-Host '[7/9] manifest + notes...'
$RelDir = Join-Path $Root 'dist\release'
New-Item -ItemType Directory -Force -Path $RelDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $RelDir 'notes') | Out-Null

$SetupName = "NovaFile-Setup-$Ver.exe"
$NotesName = "notes/$Ver.html"
if ($BaseUrl -eq '') {
    $Url = ''
    $NotesUrl = ''
    Write-Host '      base_url empty: manifest URLs left empty (configure before hosting)'
} else {
    $Url = "$BaseUrl$SetupName"
    $NotesUrl = "$BaseUrl$NotesName"
}
$Manifest = @"
{
  "schema": "1",
  "product": "NovaFile",
  "version": "$Ver",
  "url": "$Url",
  "sha256": "$Hash",
  "size": $Size,
  "notes": "$NotesUrl",
  "min_version": "$($Cfg['min_version'])"
}
"@
$Manifest | Set-Content -LiteralPath (Join-Path $RelDir 'latest.json') -Encoding ASCII
$Manifest | Write-Host

$NotesHtml = @"
<!doctype html>
<html><head><meta charset="utf-8"><title>NovaFile Explorer $Ver</title></head>
<body style="font-family:sans-serif;max-width:40em;margin:3em auto">
<h1>NovaFile Explorer $Ver</h1>
<p>Released: $(Get-Date -Format 'yyyy-MM-dd')</p>
<h2>Changes</h2>
<ul>
<li>Describe the changes of this release here before publishing.</li>
</ul>
<h2>Checksum</h2>
<p>SHA-256 of ${SetupName}:<br><code>$Hash</code></p>
</body></html>
"@
$NotesHtml | Set-Content -LiteralPath (Join-Path $RelDir "notes\$Ver.html") -Encoding ASCII
$Hash | Out-File -LiteralPath (Join-Path $RelDir 'SHA256SUMS.txt') -Encoding ascii
Add-Content -LiteralPath (Join-Path $RelDir 'SHA256SUMS.txt') -Value "  $SetupName" -Encoding ascii
Copy-Item -LiteralPath $Setup -Destination (Join-Path $RelDir $SetupName) -Force

# ---- 8. self-verify ---------------------------------------------------
Write-Host '[8/9] verify staged artifacts...'
$rehash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $RelDir $SetupName)).Hash.ToLower()
if ($rehash -ne $Hash) { Fail 'staged installer hash mismatch' }
$man = Get-Content (Join-Path $RelDir 'latest.json') -Raw | ConvertFrom-Json
if ($man.version -ne $Ver -or $man.sha256 -ne $Hash -or [long]$man.size -ne $Size) {
    Fail 'manifest does not match the staged installer'
}
Write-Host '    verified: installer hash == manifest sha256 == staged copy'

# ---- 9. publish / stage ----------------------------------------------
Write-Host '[9/9] publish...'
if ($NoPublish) { Write-Host '    -NoPublish: artifacts staged in dist\release only'; exit 0 }
if ($Cfg['publish_dir'] -eq '') {
    Write-Host @"

Artifacts staged in: $RelDir
  $SetupName
  latest.json
  notes\$Ver.html
  SHA256SUMS.txt

No publish_dir configured. Upload these to

    <$BaseUrl-or-your-host>/(same relative paths as above)

over HTTPS (SFTP/FTP-client/rsync of your host). Keep the manifest URL
exactly what your users' update checks expect (updates.cfg 'url=' line).
"@
    exit 0
}
$pd = $Cfg['publish_dir']
New-Item -ItemType Directory -Force -Path $pd | Out-Null
robocopy $RelDir $pd /E /NFL /NDL /NJH | Out-Null
if ($LASTEXITCODE -ge 8) { Fail "robocopy to $pd failed (code $LASTEXITCODE)" }
Write-Host "    published to $pd"
exit 0
