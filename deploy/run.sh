#!/usr/bin/env bash
# Thin wrapper: build (if its Dockerfile changed) and run the deploy toolbox
# container, forwarding all arguments to the on-deploy entrypoint.
#
#   export OP_SERVICE_ACCOUNT_TOKEN=ops_...
#   ./deploy/run.sh secrets check                       # read-only: prod unless named
#   ./deploy/run.sh --deployment staging infra plan
#   ./deploy/run.sh --deployment staging app deploy --tag sha-abc1234
#   ./deploy/run.sh --deployment prod app deploy --promote staging
#
# A command that changes a deployment names it with --deployment (prod or
# staging): there is no default and no environment variable for it.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# When stdout is captured (e.g. `gh secret set -b "$(./run.sh --deployment prod infra output -raw …)"`),
# disable the container's pseudo-TTY so the value isn't mangled with CR/ANSI bytes
# that break downstream consumers (a stray CR in an AWS key crashes SigV4 signing).
# Keep the TTY for interactive terminals so progress/colors still render (and
# `app shell` and `db psql` get one).
# A piped stdin (`backup import -`) needs the same: no pseudo-TTY, so the
# bytes reach the toolbox as they are.
tty_flag=""
{ [ -t 0 ] && [ -t 1 ]; } || tty_flag="-T"

# A changed deploy/Dockerfile rebuilds the image (cached otherwise), quietly
# and apart, so no build line ever reaches the command's stdout (a captured
# value, a dump on a pipe).
docker compose -f "$here/docker-compose.deploy.yml" build --quiet deploy >&2
exec docker compose -f "$here/docker-compose.deploy.yml" run --rm $tty_flag deploy "$@"
