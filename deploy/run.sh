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

# When stdout is captured (e.g. `gh secret set -b "$(./run.sh infra output -raw …)"`),
# disable the container's pseudo-TTY so the value isn't mangled with CR/ANSI bytes
# that break downstream consumers (a stray CR in an AWS key crashes SigV4 signing).
# Keep the TTY for interactive terminals so progress/colors still render.
tty_flag=""
[ -t 1 ] || tty_flag="-T"

exec docker compose -f "$here/docker-compose.deploy.yml" run --rm $tty_flag deploy "$@"
