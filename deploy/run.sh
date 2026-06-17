#!/usr/bin/env bash
# Thin wrapper: build (if needed) and run the deploy toolbox container,
# forwarding all arguments to the on-deploy entrypoint.
#
#   export OP_SERVICE_ACCOUNT_TOKEN=ops_...
#   ./deploy/run.sh secrets check
#   ./deploy/run.sh infra plan
#   ./deploy/run.sh infra apply
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec docker compose -f "$here/docker-compose.deploy.yml" run --rm deploy "$@"
