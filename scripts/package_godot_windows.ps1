# Build the minimal Windows game ZIPs (one per game: OpenNova's own, Joint Operations and
# DFX2), the editor ZIP and the ONRUNTIME update package. Each game zip is the same runtime
# with the wizards naming its game; update.exe installs that game on the first launch, so this
# script needs no content checkout.
# Usage: pwsh -File scripts/package_godot_windows.ps1 [-ExportMode release|debug] [-SkipBuild]
#        [-PreviousRuntimePackage <prior.sfs>] [-Version <n.n.n>]

param(
    # The version the packages carry: the zips' names, the exes' VERSIONINFO, the
    # project's config/version in the exports and every update package's VERSION
    # (ADR 0067 d8: the one a player is told). release.yml passes the release tag
    # without its "v"; empty (PR CI, local runs) takes godot/project.godot's
    # config/version.
    [string]$Version = "",
    # CI builds the GDExtension once per flavour (the build-gdextension-windows job)
    # and downloads the DLLs into godot\bin; -SkipBuild then skips the per-job
    # rebuild. Local runs omit it and build normally. See
    # .github/workflows/ci.yml build-gdextension-windows.
    [switch]$SkipBuild,
    # Carry removals from the previous full runtime release so any older install converges.
    [string]$PreviousRuntimePackage = "",
    # Godot export mode. "release" (default; what the release workflow ships):
    # --export-release, and the packaged exes load the template_release
    # GDExtensions. "debug" (pull-request CI): --export-debug, and the packaged exes
    # load the template_debug GDExtensions — the development flavour — so a PR
    # only has to compile one flavour. The Godot editor itself
    # always loads template_debug while it scans scripts for the export, so that
    # DLL is required in both modes.
    [ValidateSet("debug", "release")]
    [string]$ExportMode = "release"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # keeps Invoke-WebRequest fast on large files

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

# Install the ImGui bridge: the game's dev tools (debug) and the editor's windows.
Write-Host "=== Installing the imgui-godot addon ==="
& bash "scripts/bootstrap_imgui_godot.sh"
if ($LASTEXITCODE -ne 0) { throw "bootstrap_imgui_godot.sh failed (exit $LASTEXITCODE)" }

# The packages' version: -Version (a release's tag), else Godot apps' component version
# from project.godot.
if ($Version) {
    if ($Version -notmatch '^\d+(\.\d+){0,3}$') { throw "-Version must be one to four numbers joined by dots, not '$Version'" }
} else {
    $projectGodot = Get-Content "$ROOT\godot\project.godot" -Raw
    if ($projectGodot -notmatch '(?m)^config/version\s*=\s*"([^"]+)"') {
        throw "Could not extract config/version from godot/project.godot"
    }
    $Version = $Matches[1]
}
Write-Host "=== Godot apps version: $Version ==="

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
        -DGODOTCPP_USE_STATIC_CPP=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
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
# The deployment a package talks to is its export mode's: a release package
# prod's, a debug one (PR CI's) staging's (DEPLOY.md "Deployments: dev, staging
# and prod"). The exported game's NovaWorld target picks the same deployment's
# gate by its build (godot/game/novaworld_settings.gd), and update.exe dials the
# gateway the zip's OPENNOVA.WIZ names (apps/update/client.cpp service_gateways):
# the zip always carries that wizard, as the runtime's update package does, so
# the updater's compiled fallback, prod's, is never what a zip install dials.
$UPDATE_HOST = if ($ExportMode -eq "debug") { "update.staging.opennova.net" } else { "update.opennova.net" }
Write-Host "=== Export mode: $ExportMode (ships $(Split-Path $SHIPPED_RUNTIME_DLL -Leaf) and $(Split-Path $SHIPPED_EDITOR_DLL -Leaf); updates from $UPDATE_HOST) ==="

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
$cliBuildDir = "$ROOT\build"
# Match the native apps package: no separate Visual C++ redistributable is needed.
cmake -S $ROOT -B $cliBuildDir -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE -ne 0) { throw "CMake configure of the updater tools failed" }
cmake --build $cliBuildDir --config Release --target opennova_update opennova_revupdat opennova_update_tool
if ($LASTEXITCODE -ne 0) { throw "Building the updater tools failed" }

function Find-BuiltTool {
    param([string]$Folder, [string]$Name)
    $found = @("$cliBuildDir\apps\$Folder\$Name.exe", "$cliBuildDir\apps\$Folder\Release\$Name.exe") |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $found) { throw "Built updater tool missing: $Name.exe" }
    return $found
}
$UPDATE_EXE = Find-BuiltTool "update" "update"
$REVUPDAT_EXE = Find-BuiltTool "revupdat" "revupdat"
$UPDATE_TOOL = Find-BuiltTool "update_tool" "opennova-update-tool"
if ($PreviousRuntimePackage) {
    $PreviousRuntimePackage = (Resolve-Path -LiteralPath $PreviousRuntimePackage).Path
}

$DIST = "$ROOT\dist"
New-Item -ItemType Directory -Force -Path $DIST | Out-Null

$RUNTIME_EXE = "$DIST\opennova.exe"
$PLAY_EXE = "$DIST\play\opennova.exe"
$EDITOR_EXE = "$DIST\editor\opennova-editor.exe"
$EDITOR_ZIP = "$DIST\opennova-editor-windows-v$Version.zip"
$RUNTIME_SFS = "$DIST\opennova-runtime-windows-v$Version.sfs"

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
    # A fresh minimal package reaches its embedded bootstrap without content or network:
    # OpenNova's own folder (no wizard) and the JO and DFX2 folders (their wizards) alike.
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

    Write-Host "=== Boot smoke: opennova-play-runtime ==="
    $usage = Invoke-GodotAppBoot -ExePath $PlayExe -UserArgs "--resource-dir" -ExpectedExit 2
    if ($usage -notmatch "--resource-dir needs a path") {
        Write-Host $usage.TrimEnd()
        throw "Boot smoke did not report the Play runtime's --resource-dir usage error"
    }
    Write-Host "=== Boot smoke: opennova-editor ==="
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

# A full package carries every current runtime file. Previous release removals
# are retained so clients can skip releases. Content and expansion files are
# separate products; the base builder omits expansion/ from ONRUNTIME. Its
# command file ships in the zip beside the files it lists, so a fresh install's
# first check finds the runtime current instead of fetching it again.
function New-RuntimeUpdate {
    param([string]$StageDir)
    $empty = New-StageDir ".stage-update-empty"
    $command = Join-Path $StageDir "ONRUNTIME.CMD"
    try {
        $arguments = @("build", "--from", $empty, "--to", $StageDir, "--out", $RUNTIME_SFS,
            "--base", "ONRUNTIME.CMD", "--product-name", "OpenNova Runtime", "--exe", "opennova.exe",
            "--product", "OpenNova Runtime", "--version", $Version, "--full", "--command-out", $command)
        if ($PreviousRuntimePackage) { $arguments += @("--previous", $PreviousRuntimePackage) }
        & $UPDATE_TOOL @arguments
        if ($LASTEXITCODE -ne 0) { throw "Building the full runtime update failed" }
        & $UPDATE_TOOL inspect $RUNTIME_SFS
        if ($LASTEXITCODE -ne 0) { throw "The runtime update could not be read back" }
    }
    finally { Remove-Item -LiteralPath $empty -Recurse -Force }
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

# Read-only checks against our own loopback server prove the shipped updater
# loads, speaks the protocol and reports missing files without contacting a service.
# $GameStages are the jointops and dfx2 stages with the files each stages ($Staged).
function Test-UpdaterBoot {
    param([string]$StageDir, [object[]]$GameStages)
    $smoke = New-StageDir ".stage-update-smoke"
    $server = $null
    try {
        $exe = Join-Path $StageDir "update.exe"
        Invoke-UpdaterSmoke -Exe $exe -Arguments "--help" -ExpectedExit 0 -LogDir $smoke
        Invoke-UpdaterSmoke -Exe $exe -Arguments "--check /game unknown" -ExpectedExit 2 -LogDir $smoke
        # JO's, Escalation's and DFX2's products with no rows (as the service answers an
        # unpublished retail product) prove the public current=0 result without teaching
        # this packaging script to decode SFS files.
        $empty = Join-Path $smoke "empty"
        New-Item -ItemType Directory -Path $empty | Out-Null
        $packages = @()
        foreach ($command in @("JOPTR.CMD", "JOEPTR.CMD", "DFX2.CMD")) {
            $package = Join-Path $smoke "$command.sfs"
            & $UPDATE_TOOL build --from $empty --to $empty --out $package --base $command --version 1.7.5.7 --full
            if ($LASTEXITCODE -ne 0) { throw "Building the current-product smoke fixture $command failed" }
            $packages += "--package `"$package`""
        }
        $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $probe.Start()
        $port = $probe.LocalEndpoint.Port
        $probe.Stop()
        $server = Start-Process -FilePath $UPDATE_TOOL -WindowStyle Hidden -PassThru `
            -ArgumentList "serve --package `"$RUNTIME_SFS`" $($packages -join ' ') --product ONRUNTIME=ONRUNTIME.CMD --product JOTRus=JOPTR.CMD --product JOEus=JOEPTR.CMD --product DFX2=DFX2.CMD --server-host 127.0.0.1 --port $port --loopback" `
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
        # A JO-shaped folder: JO:CA's wizard (its CMD names the product) and nothing else.
        $jo = Join-Path $smoke "jo"
        New-Item -ItemType Directory -Path $jo | Out-Null
        [IO.File]::WriteAllText((Join-Path $jo "UPDATE.WIZ"), "[HEADER]`r`nCMD=JOPTR.CMD`r`nURL=127.0.0.1`r`nPRODUCT_LOGIN=JOTRqq`r`n")
        # The staged runtime (its ONRUNTIME.CMD beside it) and the JO folder are both
        # current: 0, with /game and with the folder's wizard choosing the product.
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 0 -LogDir $smoke `
            -Arguments "--check /game jo --runtime-dir `"$StageDir`" --resource-dir `"$jo`" --server 127.0.0.1 --port $port"
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 0 -LogDir $smoke `
            -Arguments "--check --runtime-dir `"$StageDir`" --resource-dir `"$jo`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $jo -Staged @("UPDATE.WIZ")
        # A runtime directory holding only its wizard lacks every runtime row: 10.
        $bare = Join-Path $smoke "bare"
        New-Item -ItemType Directory -Path $bare | Out-Null
        Copy-Item -LiteralPath (Join-Path $StageDir "OPENNOVA.WIZ") -Destination $bare
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 10 -LogDir $smoke `
            -Arguments "--check /game jo --runtime-dir `"$bare`" --resource-dir `"$jo`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $bare -Staged @("OPENNOVA.WIZ")
        # OpenNova content never targets another game's folder: refused before any row.
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 1 -LogDir $smoke `
            -Arguments "--check /game acidgambit --runtime-dir `"$StageDir`" --resource-dir `"$jo`" --server 127.0.0.1 --port $port"
        # Each game zip as unzipped: its own update.exe, its runtime current through the
        # ONRUNTIME.CMD it carries, and its wizards naming its game's products (JO's with
        # Escalation's, DFX2's), every one current: 0, and nothing written.
        foreach ($game in $GameStages) {
            Invoke-UpdaterSmoke -Exe (Join-Path $game.Dir "update.exe") -ExpectedExit 0 -LogDir $smoke `
                -Arguments "--check --runtime-dir `"$($game.Dir)`" --resource-dir `"$($game.Dir)`" --server 127.0.0.1 --port $port"
            Assert-StageHoldsOnly -StageDir $game.Dir -Staged $game.Staged
        }
        $server.Kill()
        $server.WaitForExit()
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 1 -LogDir $smoke `
            -Arguments "--check /game jo --runtime-dir `"$bare`" --resource-dir `"$jo`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $jo -Staged @("UPDATE.WIZ")
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
        throw "The game stage holds files this script did not stage, which the zip would ship: $($unexpected -join ', ')"
    }
    $missing = @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $StageDir $_) -PathType Leaf) })
    if ($missing.Count -gt 0) {
        throw "Files this script staged are gone from the game stage: $($missing -join ', ')"
    }
    Write-Host "    the stage holds exactly the $($files.Count) files staged"
}

# The version stamped for the three exports: the export presets' file and product versions
# (Godot embeds them in each exe's VERSIONINFO) and the project's config/version, then both
# tracked files put back as they were, so a run leaves the tree as it found it.
$presetsPath = "$ROOT\godot\export_presets.cfg"
$projectPath = "$ROOT\godot\project.godot"
$presetsOriginal = [IO.File]::ReadAllText($presetsPath)
$projectOriginal = [IO.File]::ReadAllText($projectPath)
try {
    $presets = $presetsOriginal -replace 'application/file_version="[^"]*"', "application/file_version=`"$WinVersion`""
    $presets = $presets -replace 'application/product_version="[^"]*"', "application/product_version=`"$WinVersion`""
    [IO.File]::WriteAllText($presetsPath, $presets)
    [IO.File]::WriteAllText($projectPath, ($projectOriginal -replace '(?m)^config/version\s*=\s*"[^"]*"', "config/version=`"$Version`""))
    Write-Host "    file_version / product_version -> $WinVersion, config/version -> $Version"

    Write-Host "=== Exporting opennova.exe ==="
    Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE

    Write-Host "=== Exporting the editor's Play runtime ==="
    Invoke-GodotExport -PresetName "OpenNova Play Runtime" -OutputPath $PLAY_EXE

    Write-Host "=== Exporting opennova-editor.exe ==="
    Invoke-GodotExport -PresetName "OpenNova Editor" -OutputPath $EDITOR_EXE
}
finally {
    [IO.File]::WriteAllText($presetsPath, $presetsOriginal)
    [IO.File]::WriteAllText($projectPath, $projectOriginal)
}

# --- the game zips: opennova.exe + its runtime DLL + the updater pair side by side, one
#     zip per game, each with the wizards naming its game's update products. The runtime
#     files are the same in all three, so one ONRUNTIME package (built from OpenNova's
#     stage, the one with no root wizard) updates every one; each carries its command file.
#     A folder with no UPDATE.WIZ is OP: Acid Gambit's; JO's and DFX2's are the wizards
#     their stock installs ship (updater-re section 2; ADR 0067 d2), so the first launch
#     downloads that game, and Escalation registers by its wizard alone.
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
Online play: choose OpenNova in the NovaWorld menu. The OpenNova Switcher is
only needed for the original dfx2.exe, whose NovaLogic NovaWorld is down.
"@
if ($ExportMode -eq "debug") {
    $launchHelp += "`n`nThis is a development build. It updates from OpenNova's staging service,`n$UPDATE_HOST, and the NovaWorld menu's OpenNova choice plays on`nstaging's NovaWorld, nw.staging.opennova.net, not the live service."
}

# A wizard's text: the product's command file, the export mode's update host and its login.
function Get-WizardText {
    param([string]$Command, [string]$Login)
    return "[HEADER]`r`nCMD=$Command`r`nURL=$UPDATE_HOST`r`nPRODUCT_LOGIN=$Login`r`n"
}

# The game zips: their file name's prefix, their stage and the wizards each adds
# (relative path -> text). JO's and Escalation's logins carry the territory placeholder
# their stock wizards do (JOTRqq, JOEqq: JOTRus and JOEus with no cc.bin); DFX2's has none.
$GAMES = @(
    @{ Prefix = "opennova"; Wizards = [ordered]@{
        "expansion\nifty\UPDATE.WIZ" = (Get-WizardText "NIFTY.CMD" "NIFTY") } },
    @{ Prefix = "jointops"; Wizards = [ordered]@{
        "UPDATE.WIZ" = (Get-WizardText "JOPTR.CMD" "JOTRqq")
        "expansion\jox01\UPDATE.WIZ" = (Get-WizardText "JOEPTR.CMD" "JOEqq") } },
    @{ Prefix = "dfx2"; Wizards = [ordered]@{
        "UPDATE.WIZ" = (Get-WizardText "DFX2.CMD" "DFX2") } }
)

try {
    foreach ($game in $GAMES) {
        $stagePath = New-StageDir ".stage-$($game.Prefix)-game-windows"
        $game.Dir = $stagePath
        $game.Zip = "$DIST\$($game.Prefix)-game-windows-v$Version.zip"
        $staged = New-Object System.Collections.Generic.List[string]
        $game.Staged = $staged
        $stagedExe = Join-Path $stagePath "opennova.exe"
        Copy-Item -LiteralPath $RUNTIME_EXE -Destination $stagedExe
        $staged.Add("opennova.exe")
        Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $stagePath
        $staged.Add((Split-Path $SHIPPED_RUNTIME_DLL -Leaf))
        foreach ($name in @(Copy-ImGuiLibs -ExeDir $DIST -Destination $stagePath -Required ($ExportMode -eq "debug"))) {
            $staged.Add($name)
        }
        Copy-Item -LiteralPath $UPDATE_EXE, $REVUPDAT_EXE -Destination $stagePath
        $staged.Add("update.exe")
        $staged.Add("revupdat.exe")
        [IO.File]::WriteAllText((Join-Path $stagePath "OPENNOVA.WIZ"), (Get-WizardText "ONRUNTIME.CMD" "ONRUNTIME"))
        $staged.Add("OPENNOVA.WIZ")
        foreach ($wizard in $game.Wizards.GetEnumerator()) {
            $path = Join-Path $stagePath $wizard.Key
            New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force | Out-Null
            [IO.File]::WriteAllText($path, $wizard.Value)
            $staged.Add($wizard.Key)
        }
        Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $launchHelp -Encoding UTF8
        $staged.Add("README.txt")
        Test-GodotAppBoot -ExePath $stagedExe -Label "$($game.Prefix)-game-windows"
    }
    $native = $GAMES[0]
    New-RuntimeUpdate -StageDir $native.Dir
    foreach ($game in $GAMES) {
        if ($game -ne $native) {
            Copy-Item -LiteralPath (Join-Path $native.Dir "ONRUNTIME.CMD") -Destination $game.Dir
        }
        $game.Staged.Add("ONRUNTIME.CMD")
    }
    Test-UpdaterBoot -StageDir $native.Dir -GameStages @($GAMES | Where-Object { $_ -ne $native })
    foreach ($game in $GAMES) {
        Assert-StageHoldsOnly -StageDir $game.Dir -Staged $game.Staged
        Compress-Archive -Path (Join-Path $game.Dir "*") -DestinationPath $game.Zip -Force
        if (-not (Test-Path -LiteralPath $game.Zip)) { throw "Packaging produced no $($game.Prefix) zip" }
    }
}
finally {
    foreach ($game in $GAMES) {
        if ($game.Dir -and (Test-Path -LiteralPath $game.Dir)) { Remove-Item -LiteralPath $game.Dir -Recurse -Force }
    }
}

# --- the editor zip: editor/ (the editor + its DLL + ImGui) and runtime/ (the Play
#     child + the runtime DLL), the layout the editor's Play looks for ---
$stagePath = New-StageDir ".stage-opennova-editor-windows"
try {
    $editorDir = Join-Path $stagePath "editor"
    $runtimeDir = Join-Path $stagePath "runtime"
    New-Item -ItemType Directory -Path $editorDir, $runtimeDir | Out-Null
    $stagedEditorExe = Join-Path $editorDir "opennova-editor.exe"
    $stagedPlayExe = Join-Path $runtimeDir "opennova.exe"
    Copy-Item -LiteralPath $EDITOR_EXE -Destination $stagedEditorExe
    Copy-Item -LiteralPath $SHIPPED_EDITOR_DLL -Destination $editorDir
    Copy-ImGuiLibs -ExeDir (Split-Path $EDITOR_EXE -Parent) -Destination $editorDir -Required $true | Out-Null
    Copy-Item -LiteralPath $PLAY_EXE -Destination $stagedPlayExe
    Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $runtimeDir
    Copy-ImGuiLibs -ExeDir (Split-Path $PLAY_EXE -Parent) -Destination $runtimeDir -Required ($ExportMode -eq "debug") | Out-Null
    $editorHelp = @"
OpenNova Editor (experimental).

  editor\opennova-editor.exe   the editor: create or open a project, fill in the
                               files the game needs, build, play
  runtime\opennova.exe         the game the editor's Play runs your project with

Keep the two folders side by side: Play looks for runtime\opennova.exe next to
the editor folder (or set another runtime in the editor's File > Project settings...).
"@
    Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $editorHelp -Encoding UTF8
    Test-EditorAppBoot -EditorExe $stagedEditorExe -PlayExe $stagedPlayExe
    Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $EDITOR_ZIP -Force
    if (-not (Test-Path -LiteralPath $EDITOR_ZIP)) { throw "Packaging produced no editor zip" }
}
finally {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
Write-Host "=== Done ==="
Get-Item -LiteralPath (@($GAMES | ForEach-Object { $_.Zip }) + @($EDITOR_ZIP, $RUNTIME_SFS)) | Select-Object Name, Length
