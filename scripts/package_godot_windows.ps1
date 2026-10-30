# Build the two plain Windows builds the update service packs (ADR 0072): the runtime,
# opennova-runtime-windows-v<version>.zip (opennova.exe, its DLL, the updater pair and
# README.txt), and the editor, opennova-editor-windows-v<version>.zip (one flat folder: oneditor.exe
# and onplay.exe with their DLLs, the updater pair and README.txt). Neither holds a wizard or a
# command file: the service writes each app's OPENNOVA.WIZ into its package, and the downloads
# host's zips (each game's: the runtime with its game's wizards) are rendered from the published
# releases. Publish (.github/workflows/publish.yml) uploads both. The updater smoke packs them
# locally into packages it never ships. This script needs no content checkout.
# Usage: pwsh -File scripts/package_godot_windows.ps1 [-ExportMode release|debug] [-SkipBuild]
#        [-Version <n.n.n>] [-BuildCommit <sha>] [-BuildTime <unix seconds>] [-Staging]
#        [-PackageToolsDir <directory>]

param(
    # ONE's version, the one the builds carry (engine/base/version/one_build.h): the
    # GDExtensions' and the updater tools' stamp, the zips' names, the exes'
    # VERSIONINFO, the project's config/version in the exports (the MCP endpoint's
    # serverInfo reads it) and the smoke packages' VERSION. The service stamps each
    # release's own (ADR 0067 d8: the one a player is told). release.yml passes the
    # release tag without its "v", ci.yml its run's stamp (scripts/ci/one_stamp.sh);
    # empty (a local run) takes the latest v* tag reachable from HEAD, as CMake does.
    [string]$Version = "",
    # CI builds the GDExtension once per flavour (the build-gdextension-windows job)
    # and downloads the DLLs into godot\bin; -SkipBuild then skips the per-job
    # rebuild. Local runs omit it and build normally. See
    # .github/workflows/ci.yml build-gdextension-windows.
    [switch]$SkipBuild,
    # Godot export mode. "release" (default; what the release workflow ships):
    # --export-release, and the packaged exes load the template_release
    # GDExtensions. "debug" (pull-request CI): --export-debug, and the packaged exes
    # load the template_debug GDExtensions — the development flavour — so a PR
    # only has to compile one flavour. The Godot editor itself
    # always loads template_debug while it scans scripts for the export, so that
    # DLL is required in both modes.
    [ValidateSet("debug", "release")]
    [string]$ExportMode = "release",
    # ONE's commit, the one the build names on the main menu (D-MNU-76): CI passes a pull
    # request's head (the merge ref it checks out is another commit) or the tag's; a local run
    # takes its checkout's HEAD.
    [string]$BuildCommit = "",
    # ONE's build time, the commit's time in Unix seconds: CI passes its run's stamp (a pull
    # request's head is not in its shallow checkout); empty takes -BuildCommit's from git.
    [string]$BuildTime = "",
    # A staging build (a pull request's CI run, its stamp's staging flag).
    [switch]$Staging,
    # The shared Release/static-CRT package build's flat executables. Empty
    # keeps standalone/release callers building their own updater tools.
    [string]$PackageToolsDir = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # keeps Invoke-WebRequest fast on large files

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

# Install the ImGui bridge: the game's dev tools (debug) and the editor's windows.
Write-Host "=== Installing the imgui-godot addon ==="
& bash "scripts/bootstrap_imgui_godot.sh"
if ($LASTEXITCODE -ne 0) { throw "bootstrap_imgui_godot.sh failed (exit $LASTEXITCODE)" }

# ONE's stamp, which every build here carries (engine/base/CMakeLists.txt checks it): -Version
# (a release's tag, a CI run's stamp), else the latest v* tag reachable from HEAD without its v;
# -BuildCommit, else HEAD; -BuildTime, else that commit's time. godot/project.godot's
# config/version is no source: the exports get this version stamped over it below.
if ($Version) {
    if ($Version -notmatch '^\d+(\.\d+){0,3}$') { throw "-Version must be one to four numbers joined by dots, not '$Version'" }
} else {
    $tag = & git -C $ROOT describe --tags --abbrev=0 --match 'v[0-9]*'
    if ($LASTEXITCODE -ne 0 -or -not $tag) { throw "No -Version, and no v* tag is reachable from HEAD to take it from" }
    $Version = $tag.Trim() -replace '^v', ''
}
if (-not $BuildCommit) { $BuildCommit = (& git -C $ROOT rev-parse HEAD).Trim() }
if (-not $BuildTime) {
    $BuildTime = & git -C $ROOT log -1 --format=%ct $BuildCommit
    if ($LASTEXITCODE -ne 0 -or -not $BuildTime) { throw "No -BuildTime, and git names no commit time for $BuildCommit" }
    $BuildTime = $BuildTime.Trim()
}
$BuildCommit = $BuildCommit.Substring(0, [Math]::Min(7, $BuildCommit.Length))
$OneStamp = @("-DOPENNOVA_VERSION=$Version", "-DOPENNOVA_COMMIT=$BuildCommit",
    "-DOPENNOVA_BUILD_TIME=$BuildTime", "-DOPENNOVA_STAGING=$(if ($Staging) { 'ON' } else { 'OFF' })")
Write-Host "=== ONE $Version (commit $BuildCommit, time $BuildTime, staging $([bool]$Staging)) ==="

# Pad to 4 numeric parts for Windows VERSIONINFO (Godot requires major.minor.patch.build)
$versionParts = @($Version.Split("."))
while ($versionParts.Count -lt 4) { $versionParts += "0" }
$WinVersion = $versionParts -join "."

$GODOT_VERSION = "4.6.1-stable"
$GODOT_VERSION_PLAIN = "4.6.1"
$GODOT_ZIP_URL = "https://github.com/godotengine/godot/releases/download/$GODOT_VERSION/Godot_v${GODOT_VERSION}_win64.exe.zip"
$TEMPLATES_URL = "https://github.com/godotengine/godot/releases/download/$GODOT_VERSION/Godot_v${GODOT_VERSION}_export_templates.tpz"
$TEMPLATES_DIR = "$env:APPDATA\Godot\export_templates\${GODOT_VERSION_PLAIN}.stable"

$GODOT_DIR = "$ROOT\.godot-bin"
$GODOT_EXE = "$GODOT_DIR\Godot_v${GODOT_VERSION}_win64.exe"

# ---------------------------------------------------------------------------
# 1. Install Godot editor (skip if already present)
# ---------------------------------------------------------------------------
if (-not (Test-Path $GODOT_EXE)) {
    Write-Host "=== Downloading Godot $GODOT_VERSION ==="
    New-Item -ItemType Directory -Force -Path $GODOT_DIR | Out-Null
    $tempZip = "$GODOT_DIR\godot.zip"
    Invoke-WebRequest -Uri $GODOT_ZIP_URL -OutFile $tempZip
    Expand-Archive -Path $tempZip -DestinationPath $GODOT_DIR -Force
    Remove-Item $tempZip
}
if (-not (Test-Path $GODOT_EXE)) {
    throw "Godot executable missing after extraction: $GODOT_EXE"
}

# ---------------------------------------------------------------------------
# 2. Install export templates (skip if already present)
# ---------------------------------------------------------------------------
if (-not (Test-Path "$TEMPLATES_DIR\version.txt")) {
    Write-Host "=== Downloading export templates ==="
    New-Item -ItemType Directory -Force -Path $TEMPLATES_DIR | Out-Null
    $tempTpz = "$GODOT_DIR\templates.zip"
    Invoke-WebRequest -Uri $TEMPLATES_URL -OutFile $tempTpz
    $extractDir = "$GODOT_DIR\templates-extract"
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
    Expand-Archive -Path $tempTpz -DestinationPath $extractDir -Force
    # .tpz archives contain a "templates" root folder
    Copy-Item "$extractDir\templates\*" $TEMPLATES_DIR -Recurse -Force
    Remove-Item $tempTpz
    Remove-Item $extractDir -Recurse -Force
}

# ---------------------------------------------------------------------------
# 3. Build GDExtension binaries: both variants of one flavour per configure
#    (ADR 0046 d2): libopennova.* (runtime-only: the game, the Play child) and
#    libopennova_editor.* (the editor).
# ---------------------------------------------------------------------------
function Invoke-GDExtensionBuild {
    param(
        [string]$GodotCppTarget,
        [string]$BuildDir,
        [string]$Config,
        [string[]]$ExpectedDlls
    )

    Write-Host "=== Building $GodotCppTarget GDExtensions ==="
    cmake -S godot/src -B $BuildDir "-DGODOTCPP_TARGET=$GodotCppTarget" `
        -DGODOTCPP_USE_STATIC_CPP=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded @OneStamp
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    cmake --build $BuildDir --config $Config --target opennova opennova_editor_gdext
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    foreach ($dll in $ExpectedDlls) {
        if (-not (Test-Path $dll)) {
            throw "GDExtension DLL missing after $GodotCppTarget build: $dll"
        }
    }
}

$RUNTIME_DEBUG_DLL = "$ROOT\godot\bin\opennovad.dll"
$RUNTIME_RELEASE_DLL = "$ROOT\godot\bin\opennova.dll"
$EDITOR_DEBUG_DLL = "$ROOT\godot\bin\oneditord.dll"
$EDITOR_RELEASE_DLL = "$ROOT\godot\bin\oneditor.dll"

# DLL roles. The Godot editor loads the plain windows.debug row of
# bin/opennova.gdextension (the editor-enabled template_debug library, the superset)
# while scanning scripts for ANY export, so it is always required. Each packaged exe
# loads the library its feature tag and export mode select: the game and the Play
# child the runtime-only one, the editor the editor-enabled one — and only those
# shipped libraries are boot-smoked and zipped.
$SCAN_DLL = $EDITOR_DEBUG_DLL
if ($ExportMode -eq "debug") {
    $SHIPPED_RUNTIME_DLL = $RUNTIME_DEBUG_DLL
    $SHIPPED_EDITOR_DLL = $EDITOR_DEBUG_DLL
} else {
    $SHIPPED_RUNTIME_DLL = $RUNTIME_RELEASE_DLL
    $SHIPPED_EDITOR_DLL = $EDITOR_RELEASE_DLL
}
$NeedsReleaseDll = ($ExportMode -eq "release")
# The deployment a build talks to is its export mode's: a release build prod's, a
# debug one (PR CI's, which only staging publishes) staging's (DEPLOY.md
# "Deployments: dev, staging and prod"). The exported game's NovaWorld target picks
# the deployment's gate by its build (godot/game/novaworld_settings.gd); update.exe
# dials the gateway its folder's OPENNOVA.WIZ names (apps/update/client.cpp
# service_gateways), which the deployment's service writes into the package.
Write-Host "=== Export mode: $ExportMode (ships $(Split-Path $SHIPPED_RUNTIME_DLL -Leaf) and $(Split-Path $SHIPPED_EDITOR_DLL -Leaf)) ==="

if ($SkipBuild) {
    Write-Host "=== -SkipBuild: using prebuilt GDExtension DLLs in godot\bin ==="
    foreach ($dll in @($SCAN_DLL, $RUNTIME_DEBUG_DLL)) {
        if (-not (Test-Path $dll)) { throw "Expected prebuilt debug DLL missing: $dll" }
    }
    if ($NeedsReleaseDll) {
        foreach ($dll in @($RUNTIME_RELEASE_DLL, $EDITOR_RELEASE_DLL)) {
            if (-not (Test-Path $dll)) { throw "Expected prebuilt release DLL missing: $dll" }
        }
    }
}
else {
    # RelWithDebInfo, not Debug: the same optimized-plus-symbols flavour
    # scripts/build_godot.sh Dev and CI produce, so a debug-mode package ships
    # the DLL source/debug game runs use rather than an /Od build.
    Invoke-GDExtensionBuild `
        -GodotCppTarget "template_debug" `
        -BuildDir "build-godot-debug" `
        -Config "RelWithDebInfo" `
        -ExpectedDlls @($RUNTIME_DEBUG_DLL, $EDITOR_DEBUG_DLL)

    if ($NeedsReleaseDll) {
        Invoke-GDExtensionBuild `
            -GodotCppTarget "template_release" `
            -BuildDir "build-godot-release" `
            -Config "Release" `
            -ExpectedDlls @($RUNTIME_RELEASE_DLL, $EDITOR_RELEASE_DLL)
    }
}

# ---------------------------------------------------------------------------
# 4. Run headless exports
# ---------------------------------------------------------------------------
# The updater pair carries the GDExtensions' ONE stamp. Shared CI tools already
# have it; standalone builds pass the same stamp into their static-CRT build.
if (-not $PackageToolsDir) {
    $PackageToolsDir = "$ROOT\build\package-tools"
    & bash "$ROOT/scripts/build_package_tools.sh" --out $PackageToolsDir --godot @OneStamp
    if ($LASTEXITCODE -ne 0) { throw "Building the updater tools failed" }
}

function Find-BuiltTool {
    param([string]$Name)
    $found = Join-Path $PackageToolsDir "$Name.exe"
    if (-not (Test-Path -LiteralPath $found)) { throw "Built updater tool missing: $found" }
    return $found
}
$UPDATE_EXE = Find-BuiltTool "update"
$REVUPDAT_EXE = Find-BuiltTool "revupdat"
$UPDATE_TOOL = Find-BuiltTool "opennova-update-tool"

$DIST = "$ROOT\dist"
New-Item -ItemType Directory -Force -Path $DIST | Out-Null

$RUNTIME_EXE = "$DIST\opennova.exe"
# The editor's two exports land in one folder, as its zip ships them: their PCKs are embedded and
# their names differ, and each loads its own GDExtension (oneditor[d].dll, opennova[d].dll).
$PLAY_EXE = "$DIST\editor\onplay.exe"
$EDITOR_EXE = "$DIST\editor\oneditor.exe"
$RUNTIME_ZIP = "$DIST\opennova-runtime-windows-v$Version.zip"
$EDITOR_ZIP = "$DIST\opennova-editor-windows-v$Version.zip"

function Invoke-GodotExport {
    param([string]$PresetName, [string]$OutputPath)
    New-Item -ItemType Directory -Force -Path (Split-Path $OutputPath -Parent) | Out-Null
    # Pass as a single command-line string so spaces in $PresetName survive.
    # Start-Process -Wait blocks until Godot exits; plain `&` has shown to return
    # before Godot finishes writing the .exe under some output-redirection setups.
    # --export-release / --export-debug picks the export template and, through
    # the .gdextension feature tags, which GDExtension flavour the exe loads.
    $arguments = "--headless --path godot --export-$ExportMode `"$PresetName`" `"$OutputPath`""
    $stdoutLog = [System.IO.Path]::GetTempFileName()
    $stderrLog = [System.IO.Path]::GetTempFileName()

    try {
        $proc = Start-Process `
            -FilePath $GODOT_EXE `
            -ArgumentList $arguments `
            -WindowStyle Hidden `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $stdout = Get-Content $stdoutLog -Raw
        $stderr = Get-Content $stderrLog -Raw
        $combinedOutput = "$stdout`n$stderr"

        if ($stdout) { Write-Host $stdout.TrimEnd() }
        if ($stderr) { Write-Host $stderr.TrimEnd() }

        if ($proc.ExitCode -ne 0) {
            throw "Godot export of '$PresetName' exited with code $($proc.ExitCode)"
        }

        if ($combinedOutput -match "SCRIPT ERROR: Parse Error|GDExtension dynamic library not found|No loader found for resource|Failed to load script") {
            throw "Godot export of '$PresetName' logged script or GDExtension load errors"
        }
    }
    finally {
        Remove-Item $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue
    }

    if (-not (Test-Path $OutputPath)) {
        throw "Godot export of '$PresetName' produced no file at $OutputPath"
    }
}

# Boot the staged exe headless for a few frames. This is the guard the
# packaging pipeline was missing: the export step only validates export-time
# logs, so a package whose main scene cannot load (e.g. a missing per-product
# run/main_scene feature override in project.godot) still shipped, crashing on
# first launch. Runs against the staged zip layout (exe, GDExtension DLL and
# PFF archives side by side), with a throwaway directory for everything it writes,
# so nothing lands in the stage. $UserArgs are the game's own arguments (after
# Godot's `--`). Returns the combined output.
function Invoke-GodotAppBoot {
    param([string]$ExePath, [string]$UserArgs, [int]$ExpectedExit)

    $stdoutLog = [System.IO.Path]::GetTempFileName()
    $stderrLog = [System.IO.Path]::GetTempFileName()
    $smokeTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $bootProfile = Join-Path $smokeTempRoot ("opennova-package-smoke-" + [Guid]::NewGuid().ToString("N"))
    $previousAppData = [Environment]::GetEnvironmentVariable("APPDATA", "Process")
    try {
        # This startup smoke must not discover the packager's saved retail folder,
        # credentials, or editor state.
        New-Item -ItemType Directory -Path $bootProfile | Out-Null
        [Environment]::SetEnvironmentVariable("APPDATA", $bootProfile, "Process")
        # Headless uses Godot's Dummy renderer, so a render loop cannot validate
        # GPU work here. Disable it to keep --quit-after from racing render-server
        # teardown while this smoke still loads the product scene, scripts,
        # shaders, and GDExtension. --quit-after ends the runtime's menu leg
        # with its music streaming, a forced quit that exits cleanly since the
        # SBF stream hands the mixer a built-in playback (GUT
        # audio/forced_quit_music_test.gd).
        # The game keeps its saves in its working directory, as retail does
        # (player.sav and weapon.sav, written at the first menu start;
        # LaunchFlags.working_dir). Godot's export templates move the process to
        # the exe's folder before the game runs (Main::setup), whatever folder it
        # was started in, so the smoke of the staged exe saved into the stage and
        # the 0.0.16 and 0.0.17 zips shipped the packager's profile, its Windows
        # login the callsign. --working-dir names the throwaway profile instead,
        # as the editor's Play names its run directory, and the process starts
        # there too. user:// sits under APPDATA, above. The bootstrap still checks
        # the exe's folder, so the staged minimal layout is what boots.
        $proc = Start-Process `
            -FilePath $ExePath `
            -ArgumentList "--headless --disable-render-loop --disable-crash-handler --quit-after 120 --verbose -- --no-update --working-dir `"$bootProfile`" $UserArgs" `
            -WorkingDirectory $bootProfile `
            -WindowStyle Hidden `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $combinedOutput = "$(Get-Content $stdoutLog -Raw)`n$(Get-Content $stderrLog -Raw)"
        if ($proc.ExitCode -ne $ExpectedExit) {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke ($UserArgs) exited with code $($proc.ExitCode), expected $ExpectedExit"
        }
        if ($combinedOutput -match "Failed loading scene|Cannot open file 'res://|SCRIPT ERROR|GDExtension dynamic library not found|Failed to load script") {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke ($UserArgs) logged load errors"
        }
        return $combinedOutput
    }
    finally {
        [Environment]::SetEnvironmentVariable("APPDATA", $previousAppData, "Process")
        Remove-Item -LiteralPath $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $bootProfile) {
            $resolvedProfile = (Resolve-Path -LiteralPath $bootProfile).Path
            $tempPrefix = $smokeTempRoot.TrimEnd('\') + '\'
            if (-not $resolvedProfile.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
                ((Get-Item -LiteralPath $bootProfile).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Refusing to remove unexpected smoke profile path: $resolvedProfile"
            }
            Remove-Item -LiteralPath $resolvedProfile -Recurse -Force
        }
    }
}

function Test-GodotAppBoot {
    param([string]$ExePath, [string]$Label)

    Write-Host "=== Boot smoke: $Label ==="
    # A fresh minimal package reaches its embedded bootstrap without content or network.
    $bootstrap = Invoke-GodotAppBoot -ExePath $ExePath -UserArgs "--no-update" -ExpectedExit 1
    if ($bootstrap -notmatch "OpenNova: content installation required") {
        Write-Host $bootstrap.TrimEnd()
        throw "Boot smoke did not reach the content bootstrap"
    }
    # --resource-dir without a value stays a usage error.
    $usage = Invoke-GodotAppBoot -ExePath $ExePath -UserArgs "--resource-dir" -ExpectedExit 2
    if ($usage -notmatch "--resource-dir needs a path") {
        Write-Host $usage.TrimEnd()
        throw "Boot smoke did not report the --resource-dir usage error"
    }
}

# The editor zip's two exes (ADR 0046 d1). The Play child ships no game data (Play
# always hands it --resource-dir), so it proves its scene and variant load
# through the same usage error; the editor loads its variant and says so.
function Test-EditorAppBoot {
    param([string]$EditorExe, [string]$PlayExe)

    Write-Host "=== Boot smoke: onplay ==="
    $usage = Invoke-GodotAppBoot -ExePath $PlayExe -UserArgs "--resource-dir" -ExpectedExit 2
    if ($usage -notmatch "--resource-dir needs a path") {
        Write-Host $usage.TrimEnd()
        throw "Boot smoke did not report the Play runtime's --resource-dir usage error"
    }
    Write-Host "=== Boot smoke: oneditor ==="
    $editor = Invoke-GodotAppBoot -ExePath $EditorExe -UserArgs "--editor-smoke" -ExpectedExit 0
    if ($editor -notmatch "OpenNova Editor: smoke ok") {
        Write-Host $editor.TrimEnd()
        throw "Boot smoke did not report 'OpenNova Editor: smoke ok'"
    }
}

# A staging directory under dist/, wiped and recreated; never a data directory.
function New-StageDir {
    param([string]$Name)
    $stage = Join-Path $DIST $Name
    $stagePath = [IO.Path]::GetFullPath($stage)
    $distPrefix = [IO.Path]::GetFullPath($DIST).TrimEnd('\') + '\'
    if (-not $stagePath.StartsWith($distPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected stage path: $stagePath"
    }
    if (Test-Path -LiteralPath $stagePath) {
        if ((Get-Item -LiteralPath $stagePath).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to remove a linked stage directory: $stagePath"
        }
        Remove-Item -LiteralPath $stagePath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stagePath | Out-Null
    return $stagePath
}

# The ImGui addon's native library the export placed beside an exe (the game's
# debug tools; the editor's windows in every mode). Returns each file's name.
function Copy-ImGuiLibs {
    param([string]$ExeDir, [string]$Destination, [bool]$Required)
    if (-not $Required) { return @() }
    $name = if ($ExportMode -eq "debug") { "imguid.dll" } else { "imgui.dll" }
    $library = Join-Path $ExeDir $name
    if (-not (Test-Path -LiteralPath $library)) { throw "Export in $ExeDir is missing $name" }
    Copy-Item -LiteralPath $library -Destination $Destination
    return @($name)
}

# A local update package of a shipped stage, never shipped, as the service packs an updatable
# app's plain build (ADR 0072): the stage's files and the OPENNOVA.WIZ naming $Login, from empty,
# every file a row. $Folder is left as the app's download unzips: those files, the wizard and
# the command file, so a check there finds the app current.
function New-SmokePackage {
    param([string]$StageDir, [string]$Folder, [string]$Empty, [string]$Login, [string]$Command,
        [string]$Exe, [string]$Name, [string]$Package)
    New-Item -ItemType Directory -Path $Folder | Out-Null
    Copy-Item -Path (Join-Path $StageDir "*") -Destination $Folder
    [IO.File]::WriteAllText((Join-Path $Folder "OPENNOVA.WIZ"),
        "[HEADER]`r`nCMD=$Command`r`nURL=127.0.0.1`r`nPRODUCT_LOGIN=$Login`r`n")
    $arguments = @("build", "--from", $Empty, "--to", $Folder, "--out", $Package,
        "--base", $Command, "--product-name", $Name, "--exe", $Exe,
        "--product", $Name, "--version", $Version, "--full", "--command-out", (Join-Path $Folder $Command))
    & $UPDATE_TOOL @arguments
    if ($LASTEXITCODE -ne 0) { throw "Building the $Login smoke package failed" }
    & $UPDATE_TOOL inspect $Package
    if ($LASTEXITCODE -ne 0) { throw "The $Login smoke package could not be read back" }
}

function Invoke-UpdaterSmoke {
    param([string]$Exe, [string]$Arguments, [int]$ExpectedExit, [string]$LogDir)
    $stdout = Join-Path $LogDir "stdout.log"
    $stderr = Join-Path $LogDir "stderr.log"
    # Own the process handle from creation. Windows PowerShell's Start-Process
    # can lose the exit code of a GUI-subsystem program that exits immediately.
    $proc = [Diagnostics.Process]::new()
    $proc.StartInfo.FileName = $Exe
    $proc.StartInfo.Arguments = $Arguments
    $proc.StartInfo.UseShellExecute = $false
    $proc.StartInfo.CreateNoWindow = $true
    $proc.StartInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $proc.StartInfo.RedirectStandardOutput = $true
    $proc.StartInfo.RedirectStandardError = $true
    if (-not $proc.Start()) { $proc.Dispose(); throw "Could not start updater smoke" }
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    try {
        if (-not $proc.WaitForExit(30000)) { throw "Updater smoke timed out: $Arguments" }
        $proc.WaitForExit()
        [IO.File]::WriteAllText($stdout, $stdoutTask.GetAwaiter().GetResult())
        [IO.File]::WriteAllText($stderr, $stderrTask.GetAwaiter().GetResult())
        if ($proc.ExitCode -ne $ExpectedExit) {
            throw "Updater smoke exit $($proc.ExitCode), expected ${ExpectedExit}: $([IO.File]::ReadAllText($stdout)) $([IO.File]::ReadAllText($stderr))"
        }
    }
    finally { if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }; $proc.Dispose() }
}

# Read-only checks against our own loopback server prove the shipped updaters load, speak the
# protocol, tell an app's folder from the runtime's and report missing files, without
# contacting a service. They run on smoke copies (New-SmokePackage): the stages stay as staged.
function Test-UpdaterBoot {
    param([string]$RuntimeStage, [string]$EditorStage)
    $smoke = New-StageDir ".stage-update-smoke"
    $server = $null
    try {
        $exe = Join-Path $RuntimeStage "update.exe"
        Invoke-UpdaterSmoke -Exe $exe -Arguments "--help" -ExpectedExit 0 -LogDir $smoke
        # A /game the folder does not name: refused by the folder (exit 1), since a player's game names its own (ADR 0074).
        Invoke-UpdaterSmoke -Exe $exe -Arguments "--check /game unknown" -ExpectedExit 1 -LogDir $smoke
        $empty = Join-Path $smoke "empty"
        New-Item -ItemType Directory -Path $empty | Out-Null
        $runtime = Join-Path $smoke "runtime"
        $editor = Join-Path $smoke "editor"
        $runtimePackage = Join-Path $smoke "ONRUNTIME.sfs"
        $editorPackage = Join-Path $smoke "ONEDITOR.sfs"
        New-SmokePackage -StageDir $RuntimeStage -Folder $runtime -Empty $empty -Login "ONRUNTIME" `
            -Command "ONRUNTIME.CMD" -Exe "opennova.exe" -Name "OpenNova Runtime" -Package $runtimePackage
        New-SmokePackage -StageDir $EditorStage -Folder $editor -Empty $empty -Login "ONEDITOR" `
            -Command "ONEDITOR.CMD" -Exe "oneditor.exe" -Name "OpenNova Editor" -Package $editorPackage
        # OP: Acid Gambit's product with no rows, as the service answers an unpublished game: the
        # runtime folder's game, current.
        $gamePackage = Join-Path $smoke "ACIDGAMBIT.sfs"
        & $UPDATE_TOOL build --from $empty --to $empty --out $gamePackage --base ACIDGAMBIT.CMD --version $Version --full
        if ($LASTEXITCODE -ne 0) { throw "Building the current-game smoke fixture failed" }
        $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $probe.Start()
        $port = $probe.LocalEndpoint.Port
        $probe.Stop()
        $server = Start-Process -FilePath $UPDATE_TOOL -WindowStyle Hidden -PassThru `
            -ArgumentList "serve --package `"$runtimePackage`" --package `"$editorPackage`" --package `"$gamePackage`" --product ONRUNTIME=ONRUNTIME.CMD --product ONEDITOR=ONEDITOR.CMD --product ACIDGAMBIT=ACIDGAMBIT.CMD --server-host 127.0.0.1 --port $port --loopback" `
            -RedirectStandardOutput (Join-Path $smoke "server.log") -RedirectStandardError (Join-Path $smoke "server-error.log")
        $ready = $false
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while (-not $ready -and [DateTime]::UtcNow -lt $deadline -and -not $server.HasExited) {
            $socket = [Net.Sockets.TcpClient]::new()
            try { $socket.Connect("127.0.0.1", $port); $ready = $true }
            catch { Start-Sleep -Milliseconds 100 }
            finally { $socket.Dispose() }
        }
        if (-not $ready) { throw "Loopback updater smoke server did not start" }
        # The runtime's folder as its download unzips (its files, OPENNOVA.WIZ and ONRUNTIME.CMD),
        # its own update.exe run there with no /game: the runtime and OP: Acid Gambit, current, 0.
        Invoke-UpdaterSmoke -Exe (Join-Path $runtime "update.exe") -ExpectedExit 0 -LogDir $smoke `
            -Arguments "--check --server 127.0.0.1 --port $port"
        # The editor's likewise, its OPENNOVA.WIZ naming ONEDITOR: the app alone, current, 0.
        Invoke-UpdaterSmoke -Exe (Join-Path $editor "update.exe") -ExpectedExit 0 -LogDir $smoke `
            -Arguments "--check --server 127.0.0.1 --port $port"
        # An editor folder holding only its wizard lacks every row: 10, and nothing written.
        $bare = Join-Path $smoke "bare"
        New-Item -ItemType Directory -Path $bare | Out-Null
        Copy-Item -LiteralPath (Join-Path $editor "OPENNOVA.WIZ") -Destination $bare
        $editorUpdater = Join-Path $EditorStage "update.exe"
        Invoke-UpdaterSmoke -Exe $editorUpdater -ExpectedExit 10 -LogDir $smoke `
            -Arguments "--check --runtime-dir `"$bare`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $bare -Staged @("OPENNOVA.WIZ")
        $server.Kill()
        $server.WaitForExit()
        Invoke-UpdaterSmoke -Exe $editorUpdater -ExpectedExit 1 -LogDir $smoke `
            -Arguments "--check --runtime-dir `"$bare`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $bare -Staged @("OPENNOVA.WIZ")
    }
    finally {
        if ($null -ne $server) {
            if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
            $server.Dispose()
        }
        Remove-Item -LiteralPath $smoke -Recurse -Force
    }
}

# The zip ships exactly the files this script staged. Anything else in the
# stage fails the package, named: a save or log the boot smoke wrote (the
# 0.0.16 and 0.0.17 zips shipped the CI runner's player.sav and weapon.sav,
# which also overwrite a player's own on unzipping over an install) or a file
# any other step left there. $Staged holds paths relative to $StageDir.
function Assert-StageHoldsOnly {
    param([string]$StageDir, [string[]]$Staged)

    $prefix = [IO.Path]::GetFullPath($StageDir).TrimEnd('\') + '\'
    $files = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $dirs = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($rel in $Staged) {
        $rel = $rel -replace '/', '\'
        [void]$files.Add($rel)
        for ($dir = Split-Path $rel -Parent; $dir; $dir = Split-Path $dir -Parent) { [void]$dirs.Add($dir) }
    }
    $unexpected = @(Get-ChildItem -LiteralPath $StageDir -Recurse -Force | ForEach-Object {
        $rel = $_.FullName.Substring($prefix.Length)
        if ($_.PSIsContainer) {
            if (-not $dirs.Contains($rel)) { "$rel\" }
        }
        elseif (-not $files.Contains($rel)) { $rel }
    })
    if ($unexpected.Count -gt 0) {
        throw "The stage holds files this script did not stage, which the zip would ship: $($unexpected -join ', ')"
    }
    $missing = @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $StageDir $_) -PathType Leaf) })
    if ($missing.Count -gt 0) {
        throw "Files this script staged are gone from the stage: $($missing -join ', ')"
    }
    Write-Host "    the stage holds exactly the $($files.Count) files staged"
}

# ONE's version stamped for the three exports: the export presets' file and product versions
# (Godot embeds them in each exe's VERSIONINFO) and the project's config/version (the MCP
# endpoint's serverInfo). The STARTUP screen's VERSION label reads none of them: it names the
# GDExtension's own stamp (base/version/one_build.h, D-MNU-76). Both tracked files are put back as
# they were once the exports end, so a run leaves the tree as it found it.
$presetsPath = "$ROOT\godot\export_presets.cfg"
$projectPath = "$ROOT\godot\project.godot"
$presetsOriginal = [IO.File]::ReadAllText($presetsPath)
$projectOriginal = [IO.File]::ReadAllText($projectPath)
if ($projectOriginal -notmatch '(?m)^config/version\s*=.*$') { throw "project.godot names no config/version to stamp" }
try {
    $presets = $presetsOriginal -replace 'application/file_version="[^"]*"', "application/file_version=`"$WinVersion`""
    $presets = $presets -replace 'application/product_version="[^"]*"', "application/product_version=`"$WinVersion`""
    [IO.File]::WriteAllText($presetsPath, $presets)
    $project = $projectOriginal -replace '(?m)^config/version\s*=\s*"[^"]*"', "config/version=`"$Version`""
    [IO.File]::WriteAllText($projectPath, $project)
    Write-Host "    file_version / product_version -> $WinVersion, config/version -> $Version"

    Write-Host "=== Exporting opennova.exe ==="
    Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE

    Write-Host "=== Exporting onplay.exe, the editor's Play runtime ==="
    Invoke-GodotExport -PresetName "OpenNova Play Runtime" -OutputPath $PLAY_EXE

    Write-Host "=== Exporting oneditor.exe ==="
    Invoke-GodotExport -PresetName "OpenNova Editor" -OutputPath $EDITOR_EXE
}
finally {
    [IO.File]::WriteAllText($presetsPath, $presetsOriginal)
    [IO.File]::WriteAllText($projectPath, $projectOriginal)
}

# --- the runtime's plain build: opennova.exe + its runtime DLL + the updater pair side by side
#     and README.txt, no wizard and no command file. The service packs it as ONRUNTIME (with
#     the OPENNOVA.WIZ it writes), and each game's download is that package with the wizards
#     naming the game's products (ADR 0072): a folder with no UPDATE.WIZ is OP: Acid Gambit's;
#     JO's and DFX2's carry the wizards their stock installs ship (updater-re section 2; ADR
#     0067 d2), so the first launch downloads that game. README.txt ships in every one of them.
$launchHelp = @"
OpenNova (pre-1.0, experimental)

Unzip into an empty folder of its own and run opennova.exe. The first launch
downloads the game this folder is set up for; later launches play offline and
show UPDATE only when an update exists. Keep update.exe and revupdat.exe beside
opennova.exe.

  opennova-game-windows   OpenNova's own game, OP: Acid Gambit, and its
                          expansion, OP: Nifty Package
  jointops-game-windows   Joint Operations, with its Escalation expansion
  dfx2-game-windows       Delta Force Xtreme 2

The folder's UPDATE.WIZ says which game it holds (OpenNova's own folder has none
until its first download). To play a Joint Operations or Delta Force Xtreme 2
you already have installed, put these files in that game's folder and run
opennova.exe. To choose a game and folder yourself:
  opennova.exe -- /game jo --resource-dir "C:\Games\Joint Operations"
  opennova.exe -- /game dfx2 --resource-dir "C:\Games\Delta Force Xtreme 2"
With no --resource-dir, game data is read from the executable's folder.
  opennova.exe -- /exp nifty
  opennova.exe -- /exp jox01

Updates include the OpenNova runtime, the selected game and its installed
expansions. --no-update disables checks and installation for this launch.
Online play: choose OpenNova in the NovaWorld menu.
"@
if ($ExportMode -eq "debug") {
    $launchHelp += "`n`nThis is a development build. It updates from OpenNova's staging service,`nupdate.staging.opennova.net, and the NovaWorld menu's OpenNova choice plays on`nstaging's NovaWorld, nw.staging.opennova.net, not the live service."
}

# --- the editor's plain build: one flat folder, the editor (its DLL and ImGui) and beside it
#     the Play child (onplay.exe and the runtime DLL), where the editor's Play looks for it,
#     the updater pair and README.txt, no wizard and no command file. The service packs it as
#     ONEDITOR, whose OPENNOVA.WIZ makes the folder the editor's: update.exe there updates the
#     editor alone and starts oneditor.exe again. In a debug export the Play child's ImGui
#     library is the editor's own file (the addon's imguid.dll); a release Play export carries
#     none (the preset's imgui/release). ---
$editorHelp = @"
OpenNova Editor (pre-1.0, experimental).

  oneditor.exe   the editor: create or open a project, fill in the files the game
                 needs, build, play
  onplay.exe     the game the editor's Play runs your project with

Keep the folder's files together: Play runs the onplay.exe beside oneditor.exe
(or set another runtime in the editor's File > Project settings...), and
Help > Update OpenNova Editor... updates the editor in place through update.exe
and revupdat.exe.
"@

$runtimeStage = New-StageDir ".stage-opennova-runtime-windows"
$editorStage = New-StageDir ".stage-opennova-editor-windows"
try {
    $runtimeStaged = New-Object System.Collections.Generic.List[string]
    $stagedExe = Join-Path $runtimeStage "opennova.exe"
    Copy-Item -LiteralPath $RUNTIME_EXE -Destination $stagedExe
    $runtimeStaged.Add("opennova.exe")
    Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $runtimeStage
    $runtimeStaged.Add((Split-Path $SHIPPED_RUNTIME_DLL -Leaf))
    foreach ($name in @(Copy-ImGuiLibs -ExeDir $DIST -Destination $runtimeStage -Required ($ExportMode -eq "debug"))) {
        $runtimeStaged.Add($name)
    }
    Copy-Item -LiteralPath $UPDATE_EXE, $REVUPDAT_EXE -Destination $runtimeStage
    $runtimeStaged.Add("update.exe")
    $runtimeStaged.Add("revupdat.exe")
    Set-Content -LiteralPath (Join-Path $runtimeStage "README.txt") -Value $launchHelp -Encoding UTF8
    $runtimeStaged.Add("README.txt")
    Test-GodotAppBoot -ExePath $stagedExe -Label "opennova-runtime-windows"

    $editorStaged = New-Object System.Collections.Generic.List[string]
    $stagedEditorExe = Join-Path $editorStage "oneditor.exe"
    $stagedPlayExe = Join-Path $editorStage "onplay.exe"
    Copy-Item -LiteralPath $EDITOR_EXE -Destination $stagedEditorExe
    $editorStaged.Add("oneditor.exe")
    Copy-Item -LiteralPath $SHIPPED_EDITOR_DLL -Destination $editorStage
    $editorStaged.Add((Split-Path $SHIPPED_EDITOR_DLL -Leaf))
    foreach ($name in @(Copy-ImGuiLibs -ExeDir (Split-Path $EDITOR_EXE -Parent) -Destination $editorStage -Required $true)) {
        $editorStaged.Add($name)
    }
    Copy-Item -LiteralPath $PLAY_EXE -Destination $stagedPlayExe
    $editorStaged.Add("onplay.exe")
    Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $editorStage
    $editorStaged.Add((Split-Path $SHIPPED_RUNTIME_DLL -Leaf))
    Copy-Item -LiteralPath $UPDATE_EXE, $REVUPDAT_EXE -Destination $editorStage
    $editorStaged.Add("update.exe")
    $editorStaged.Add("revupdat.exe")
    Set-Content -LiteralPath (Join-Path $editorStage "README.txt") -Value $editorHelp -Encoding UTF8
    $editorStaged.Add("README.txt")
    Test-EditorAppBoot -EditorExe $stagedEditorExe -PlayExe $stagedPlayExe

    Test-UpdaterBoot -RuntimeStage $runtimeStage -EditorStage $editorStage
    Assert-StageHoldsOnly -StageDir $runtimeStage -Staged $runtimeStaged
    Compress-Archive -Path (Join-Path $runtimeStage "*") -DestinationPath $RUNTIME_ZIP -Force
    if (-not (Test-Path -LiteralPath $RUNTIME_ZIP)) { throw "Packaging produced no runtime zip" }
    Assert-StageHoldsOnly -StageDir $editorStage -Staged $editorStaged
    Compress-Archive -Path (Join-Path $editorStage "*") -DestinationPath $EDITOR_ZIP -Force
    if (-not (Test-Path -LiteralPath $EDITOR_ZIP)) { throw "Packaging produced no editor zip" }
}
finally {
    foreach ($stage in @($runtimeStage, $editorStage)) {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    }
}
Write-Host "=== Done ==="
Get-Item -LiteralPath @($RUNTIME_ZIP, $EDITOR_ZIP) | Select-Object Name, Length
