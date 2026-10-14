#!/usr/bin/env bash
# Thin wrapper: build (if needed) and run the deploy toolbox container,
# forwarding all arguments to the on-deploy entrypoint.
#
#   export OP_SERVICE_ACCOUNT_TOKEN=ops_...
#   ./deploy/run.sh secrets check
#   ./deploy/run.sh infra plan
#   ./deploy/run.sh infra apply
#
# HOSTED_DATA_DIR=<folder> mounts that folder read-only at /hosted-data, where
# `hosted data-snapshot --data-set <slug>` reads that data set's game files, one
# data set (one folder) per run (DEPLOY.md, "Hosted game servers"):
#   HOSTED_DATA_DIR="D:/SteamLibrary/steamapps/common/Joint Operations Combined Arms" \
#     ./deploy/run.sh hosted data-snapshot --data-set jop_2_consumer
#   HOSTED_DATA_DIR="D:/Games/Delta Force Xtreme 2" \
#     ./deploy/run.sh hosted data-snapshot --data-set dfx2_consumer
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# When stdout is captured (e.g. `gh secret set -b "$(./run.sh infra output -raw …)"`),
# disable the container's pseudo-TTY so the value isn't mangled with CR/ANSI bytes
# that break downstream consumers (a stray CR in an AWS key crashes SigV4 signing).
# Keep the TTY for interactive terminals so progress/colors still render.
tty_flag=""
[ -t 1 ] || tty_flag="-T"

# The data folder's mount, as one --volume=... argument. Under Git Bash, MSYS
# would rewrite the container path in it (/hosted-data) as a Windows path:
# MSYS2_ARG_CONV_EXCL keeps arguments starting --volume= as they are, and the
# folder is given as Docker takes a Windows one (D:/..., or cygpath's form).
mount=()
if [ -n "${HOSTED_DATA_DIR:-}" ]; then
  data_dir="$HOSTED_DATA_DIR"
  if command -v cygpath >/dev/null 2>&1; then data_dir="$(cygpath -m "$data_dir")"; fi
  mount=("--volume=${data_dir}:/hosted-data:ro")
  export MSYS2_ARG_CONV_EXCL="--volume="
fi

exec docker compose -f "$here/docker-compose.deploy.yml" run --rm $tty_flag ${mount[@]+"${mount[@]}"} deploy "$@"
