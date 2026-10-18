# Visual Studio 2026's C++ build tools beside the image's Visual Studio 2022
# (windows.pkr.hcl): the stable channel's Build Tools bootstrapper, checked as
# Microsoft's, its C++ workload with the recommended components (MSVC, the
# Windows SDK). Fails unless vswhere -latest then names an 18.x (2026)
# installation with the C++ tools, which is the one ilammy/msvc-dev-cmd picks.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$installer = Join-Path $env:TEMP 'vs_buildtools.exe'
Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vs/stable/vs_buildtools.exe' -OutFile $installer
$signature = Get-AuthenticodeSignature -FilePath $installer
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw "vs_buildtools.exe is not signed by Microsoft ($($signature.Status): $($signature.SignerCertificate.Subject))"
}

$arguments = @('--quiet', '--wait', '--norestart', '--nocache',
    '--add', 'Microsoft.VisualStudio.Workload.VCTools', '--includeRecommended')
$process = Start-Process -FilePath $installer -ArgumentList $arguments -Wait -PassThru
if ($process.ExitCode -notin 0, 3010) {
    throw "vs_buildtools.exe exited $($process.ExitCode)"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$latest = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
if (-not $latest -or -not "$($latest[0].installationVersion)".StartsWith('18.')) {
    throw "the newest Visual Studio with the C++ tools is '$($latest.installationVersion)', not 18.x (2026)"
}
Write-Host "Visual Studio $($latest[0].catalog.productDisplayVersion) at $($latest[0].installationPath)"
Get-ChildItem (Join-Path $latest[0].installationPath 'VC\Tools\MSVC') | ForEach-Object { Write-Host "MSVC $($_.Name)" }
Remove-Item $installer -Force
