# Thin wrapper (PowerShell): build (if needed) and run the deploy toolbox
# container, forwarding all arguments to the on-deploy entrypoint.
#
#   $env:OP_SERVICE_ACCOUNT_TOKEN = "ops_..."
#   .\deploy\run.ps1 secrets check
#   .\deploy\run.ps1 infra apply
#
# $env:HOSTED_DATA_DIR mounts that folder read-only at /hosted-data, where
# `hosted data-snapshot --data-set <slug>` reads that data set's game files, one
# data set (one folder) per run (DEPLOY.md, "Hosted game servers"):
#   $env:HOSTED_DATA_DIR = "D:\SteamLibrary\steamapps\common\Joint Operations Combined Arms"
#   .\deploy\run.ps1 hosted data-snapshot --data-set jop_2_consumer
#   $env:HOSTED_DATA_DIR = "D:\Games\Delta Force Xtreme 2"
#   .\deploy\run.ps1 hosted data-snapshot --data-set dfx2_consumer
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$mount = @()
if ($env:HOSTED_DATA_DIR) {
    $mount = @("--volume=$($env:HOSTED_DATA_DIR):/hosted-data:ro")
}
docker compose -f "$here\docker-compose.deploy.yml" run --rm @mount deploy @args
