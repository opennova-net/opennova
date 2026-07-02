# Thin wrapper (PowerShell): build (if needed) and run the deploy toolbox
# container, forwarding all arguments to the on-deploy entrypoint.
#
#   $env:OP_SERVICE_ACCOUNT_TOKEN = "ops_..."
#   .\deploy\run.ps1 secrets check
#   .\deploy\run.ps1 infra apply
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
docker compose -f "$here\docker-compose.deploy.yml" run --rm deploy @args
