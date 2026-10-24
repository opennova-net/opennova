# Build the minimal Windows game and editor ZIPs plus the ONRUNTIME update package.
# Content is installed by update.exe on first launch; this script needs no content checkout.
# Usage: pwsh -File scripts/package_godot_windows.ps1 [-ExportMode release|debug] [-SkipBuild]
#        [-PreviousRuntimePackage <prior.sfs>]

param(
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

# Read Godot apps' component version from project.godot
$projectGodot = Get-Content "$ROOT\godot\project.godot" -Raw
if ($projectGodot -notmatch '(?m)^config/version\s*=\s*"([^"]+)"') {
    throw "Could not extract config/version from godot/project.godot"
}
$Version = $Matches[1]
Write-Host "=== Godot apps version: $Version ==="

# Pad to 4 numeric parts for Windows VERSIONINFO (Godot requires major.minor.patch.build)
$versionParts = @($Version.Split("."))
while ($versionParts.Count -lt 4) { $versionParts += "0" }
$WinVersion = $versionParts -join "."

# Stamp version into the export presets so Godot embeds it in the .exe VERSIONINFO
$presetsPath = "$ROOT\godot\export_presets.cfg"
$presets = Get-Content $presetsPath -Raw
$presets = $presets -replace 'application/file_version="[^"]*"',    "application/file_version=`"$WinVersion`""
$presets = $presets -replace 'application/product_version="[^"]*"', "application/product_version=`"$WinVersion`""
Set-Content -Path $presetsPath -Value $presets -NoNewline
Write-Host "    file_version / product_version -> $WinVersion"

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
$GAME_ZIP = "$DIST\opennova-game-windows-v$Version.zip"
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
    param([string]$ExePath)

    Write-Host "=== Boot smoke: opennova-runtime ==="
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
# separate products; the base builder omits expansion/ from ONRUNTIME.
function New-RuntimeUpdate {
    param([string]$StageDir)
    $empty = New-StageDir ".stage-update-empty"
    try {
        $arguments = @("build", "--from", $empty, "--to", $StageDir, "--out", $RUNTIME_SFS,
            "--base", "ONRUNTIME.CMD", "--product-name", "OpenNova Runtime", "--exe", "opennova.exe",
            "--version-file", "onversion.txt", "--full")
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
function Test-UpdaterBoot {
    param([string]$StageDir)
    $smoke = New-StageDir ".stage-update-smoke"
    $server = $null
    try {
        $exe = Join-Path $StageDir "update.exe"
        Invoke-UpdaterSmoke -Exe $exe -Arguments "--help" -ExpectedExit 0 -LogDir $smoke
        Invoke-UpdaterSmoke -Exe $exe -Arguments "--check /game unknown" -ExpectedExit 2 -LogDir $smoke
        # An empty product has no pending rows, proving the public current=0
        # result without teaching this packaging script to decode SFS files.
        $empty = Join-Path $smoke "empty"
        New-Item -ItemType Directory -Path $empty | Out-Null
        $emptyPackage = Join-Path $smoke "current.sfs"
        & $UPDATE_TOOL build --from $empty --to $empty --out $emptyPackage --base ONEMPTY.CMD --full
        if ($LASTEXITCODE -ne 0) { throw "Building the current-product smoke fixture failed" }
        $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $probe.Start()
        $port = $probe.LocalEndpoint.Port
        $probe.Stop()
        $server = Start-Process -FilePath $UPDATE_TOOL -WindowStyle Hidden -PassThru `
            -ArgumentList "serve --package `"$RUNTIME_SFS`" --package `"$emptyPackage`" --product ONRUNTIME=ONRUNTIME.CMD --product ONEMPTY=ONEMPTY.CMD --server-host 127.0.0.1 --port $port --loopback" `
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
        $current = Join-Path $smoke "current"
        New-Item -ItemType Directory -Path $current | Out-Null
        $wizard = "[HEADER]`r`nCMD=ONEMPTY.CMD`r`nURL=127.0.0.1`r`nPRODUCT_LOGIN=ONEMPTY`r`n"
        [IO.File]::WriteAllText((Join-Path $current "OPENNOVA.WIZ"), $wizard)
        [IO.File]::WriteAllText((Join-Path $current "UPDATE.WIZ"), $wizard)
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 0 -LogDir $smoke `
            -Arguments "--check /game jo --runtime-dir `"$current`" --resource-dir `"$current`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $current -Staged @("OPENNOVA.WIZ", "UPDATE.WIZ")
        # The synthetic selected-game directory holds only its wizard. Both
        # runtime and selected content are checked; missing rows report 10.
        $content = Join-Path $smoke "content"
        New-Item -ItemType Directory -Path $content | Out-Null
        [IO.File]::WriteAllText((Join-Path $content "UPDATE.WIZ"), "[HEADER]`r`nCMD=ONRUNTIME.CMD`r`nURL=127.0.0.1`r`nPRODUCT_LOGIN=ONRUNTIME`r`n")
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 10 -LogDir $smoke `
            -Arguments "--check /game jo --runtime-dir `"$StageDir`" --resource-dir `"$content`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $content -Staged @("UPDATE.WIZ")
        $server.Kill()
        $server.WaitForExit()
        Invoke-UpdaterSmoke -Exe $exe -ExpectedExit 1 -LogDir $smoke `
            -Arguments "--check /game jo --runtime-dir `"$StageDir`" --resource-dir `"$content`" --server 127.0.0.1 --port $port"
        Assert-StageHoldsOnly -StageDir $content -Staged @("UPDATE.WIZ")
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

Write-Host "=== Exporting opennova.exe ==="
Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE

Write-Host "=== Exporting the editor's Play runtime ==="
Invoke-GodotExport -PresetName "OpenNova Play Runtime" -OutputPath $PLAY_EXE

Write-Host "=== Exporting opennova-editor.exe ==="
Invoke-GodotExport -PresetName "OpenNova Editor" -OutputPath $EDITOR_EXE

# --- the game zip: opennova.exe + its runtime DLL + the game's files side by side ---
$stagePath = New-StageDir ".stage-opennova-game-windows"
try {
    $staged = New-Object System.Collections.Generic.List[string]
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
    [IO.File]::WriteAllText((Join-Path $stagePath "OPENNOVA.WIZ"), "[HEADER]`r`nCMD=ONRUNTIME.CMD`r`nURL=update.opennova.net`r`nPRODUCT_LOGIN=ONRUNTIME`r`n")
    [IO.File]::WriteAllText((Join-Path $stagePath "onversion.txt"), "OpenNova Runtime, V$Version;`r`n")
    $staged.Add("OPENNOVA.WIZ")
    $staged.Add("onversion.txt")
    $expansion = Join-Path $stagePath "expansion\onjo1"
    New-Item -ItemType Directory -Path $expansion -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $expansion "UPDATE.WIZ"), "[HEADER]`r`nCMD=ONJO1.CMD`r`nURL=update.opennova.net`r`nPRODUCT_LOGIN=ONJO1`r`n")
    $staged.Add("expansion\onjo1\UPDATE.WIZ")
    $launchHelp = @"
OpenNova (pre-1.0, experimental)

Run opennova.exe. The first launch downloads OpenNova's content and registered
expansions. Later launches play offline and show UPDATE only when an update exists.
Keep update.exe and revupdat.exe beside opennova.exe.

To play Joint Operations or Delta Force Xtreme 2, put these files in that
game's folder and run opennova.exe; the folder's UPDATE.WIZ says which game
it holds. To choose a game and folder yourself:
  opennova.exe -- /game jo --resource-dir "C:\Games\Joint Operations"
  opennova.exe -- /game dfx2 --resource-dir "C:\Games\Delta Force Xtreme 2"
With no --resource-dir, game data is read from the executable's folder.
  opennova.exe -- /exp onjo1

Updates include the OpenNova runtime, selected game's content and installed
expansions. --no-update disables checks and installation for this launch.
Online play: choose OpenNova in the NovaWorld menu. The OpenNova Switcher is
only needed for the original Jointops.exe and dfx2.exe.
"@
    Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $launchHelp -Encoding UTF8
    $staged.Add("README.txt")
    Test-GodotAppBoot -ExePath $stagedExe
    New-RuntimeUpdate -StageDir $stagePath
    Test-UpdaterBoot -StageDir $stagePath
    Assert-StageHoldsOnly -StageDir $stagePath -Staged $staged
    Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $GAME_ZIP -Force
    if (-not (Test-Path -LiteralPath $GAME_ZIP)) { throw "Packaging produced no zip" }
}
finally {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
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
Get-Item -LiteralPath $GAME_ZIP, $EDITOR_ZIP, $RUNTIME_SFS | Select-Object Name, Length
