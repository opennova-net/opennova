# Thin wrapper (PowerShell): build (if its Dockerfile changed) and run the
# deploy toolbox container, forwarding all arguments to the on-deploy entrypoint.
#
#   $env:OP_SERVICE_ACCOUNT_TOKEN = "ops_..."
#   .\deploy\run.ps1 secrets check
#   .\deploy\run.ps1 --deployment staging infra apply
#   .\deploy\run.ps1 --deployment prod app deploy --promote staging
#
# A command that changes a deployment names it with --deployment (prod or
# staging): there is no default and no environment variable for it.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# The checkout's commit for a release's manifest, read here on the host: a
# linked worktree's .git names a directory outside the container's mount, so
# the container's git cannot read it. Unread (no git here), the container's
# own git tries.
if (Get-Command git -ErrorAction SilentlyContinue) {
    $rev = git -C "$here\.." rev-parse --short=12 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $rev) {
        $env:OPENNOVA_REVISION = $rev
        $env:OPENNOVA_INFRA_DIRTY = if (git -C "$here\.." status --porcelain -- infra/aws) { "true" } else { "false" }
    }
}
# A changed deploy/Dockerfile rebuilds the image (cached otherwise), quietly
# and apart, so no build line ever reaches the command's output.
docker compose -f "$here\docker-compose.deploy.yml" build --quiet deploy | Out-Host
docker compose -f "$here\docker-compose.deploy.yml" run --rm deploy @args
