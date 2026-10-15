#!/bin/bash
# opennova-converge: brings the NovaWorld service host to its deployment's
# current release (DEPLOY.md, section 4).
#
# It runs as root, two ways: through SSM Run Command, as the deployment's
# converge document, which holds this file (infra/aws/service_ssm.tf; the
# toolbox's `app deploy` and `app rollback` send it); and at every boot, from
# opennova-converge.service, which fetches the same document
# (deploy/host/service-user-data.sh). Each run:
#
#   1. reads the deployment's settings from SSM Parameter Store into
#      memory-only env files (/run is a tmpfs; root's, 0600):
#      /run/opennova/infra.env, what Terraform knows (the public address, the
#      site's URL, the hosted machines' settings, the bucket, the log group),
#      and /run/opennova/secret.env, the runtime secrets that `secrets sync`
#      copies from the vault (/opennova/<deployment>/secret/*);
#   2. reads the current release's id (/opennova/<deployment>/release) and, the
#      first time, downloads its bundle from the operations bucket into
#      /opt/opennova/releases/<id>: the compose files, the committed
#      deploy/env/app.prod.env, release.env (the images, pinned by digest) and
#      the backup sidecar's build context; /opt/opennova/current points at it;
#   3. makes sure of the RDS CA bundle the host's database tools verify
#      against (pinned, as the images pin it), and installs the helpers the toolbox's other commands run here
#      over SSM (opennova-compose, opennova-psql, opennova-admin-api) and itself
#      (/usr/local/sbin/opennova-converge);
#   4. pulls the release's images, then `docker compose up -d`: compose
#      recreates only the containers whose image or settings changed (a
#      web-only release never restarts the NovaWorld server), and waits until
#      each runs and the server answers /healthz;
#   5. prints the server's newest `[boot] migrations:` line and the containers.
#
# Runs queue on a lock. The release "none" (nothing deployed yet) converges
# nothing. A failed pull changes nothing; a release that does not come up
# healthy is left for `./deploy/run.sh app rollback`.

# SSM may start this under sh; the rest is bash.
[ -n "${BASH_VERSION:-}" ] || exec /bin/bash "$0" "$@"

set -euo pipefail
umask 077
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root

readonly BASE=/opt/opennova
readonly RELEASES=$BASE/releases
readonly RUN=/run/opennova
readonly SBIN=/usr/local/sbin
readonly KEEP_RELEASES=5

log() { printf '[converge] %s\n' "$*"; }
die() { printf '[converge] ERROR: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run it as root"
[ -r /etc/opennova/host.env ] || die "no /etc/opennova/host.env: this is no service host"
set -a
# shellcheck disable=SC1091  # written by the host's first boot
. /etc/opennova/host.env
set +a
: "${OPENNOVA_DEPLOYMENT:?}" "${OPENNOVA_SSM_PREFIX:?}"

install -d -m 0700 "$RUN"
install -d -m 0755 "$BASE" "$RELEASES"

exec 9>"$RUN/converge.lock"
flock -w 1200 9 || die "another converge held the lock for 20 minutes"

trap 'rm -f "$RUN"/params.*.json "$RUN"/secret.env.* "$RUN"/infra.env.* 2>/dev/null || true' EXIT

# Run later by name (the boot unit and SSM run a copy of the document's text).
self="$(readlink -f "$0" 2>/dev/null || true)"
if [ -n "$self" ] && [ -f "$self" ] && [ "$self" != "$SBIN/opennova-converge" ]; then
  install -m 0755 "$self" "$SBIN/opennova-converge"
fi

# A converge sent while the host's first boot still installs docker (a new
# host, an early `app deploy`) waits for it.
if command -v cloud-init >/dev/null 2>&1; then
  timeout 1200 cloud-init status --wait >/dev/null 2>&1 || true
fi
command -v docker >/dev/null 2>&1 \
  || die "docker is not installed: the host's first boot failed (/var/log/cloud-init-output.log)"

ssm_get() { aws ssm get-parameter --name "$1" --query Parameter.Value --output text; }

# A value of an env file of KEY='value' lines.
env_value() { sed -n "s/^$2='\\(.*\\)'\$/\\1/p" "$1" | tail -n 1; }

# --- 1. settings ---------------------------------------------------------------------
log "deployment $OPENNOVA_DEPLOYMENT: reading $OPENNOVA_SSM_PREFIX/infra.env"
infra="$(ssm_get "$OPENNOVA_SSM_PREFIX/infra.env")" \
  || die "cannot read $OPENNOVA_SSM_PREFIX/infra.env (./deploy/run.sh infra apply makes it)"
if printf '%s\n' "$infra" | grep -Evq "^[A-Z][A-Z0-9_]*='[^']*'\$|^\$"; then
  die "$OPENNOVA_SSM_PREFIX/infra.env holds a line that is no KEY='value'"
fi
tmp="$(mktemp "$RUN/infra.env.XXXXXX")"
printf '%s\n' "$infra" > "$tmp"
mv -f "$tmp" "$RUN/infra.env"

# Each parameter under a path as a KEY='value' line, its name's last part the
# KEY; a name that is no KEY, or a value with a quote or a newline (which the
# env file cannot carry), stops the run without printing the value.
JQ_ENV_LINES="$(cat <<'JQ'
.Parameters[]
| (.Name | split("/") | last) as $n
| if ($n | test("^[A-Z][A-Z0-9_]*$") | not) then error("parameter \(.Name): its name is no KEY")
  elif (.Value | test("['\n]")) then error("parameter \(.Name): its value holds a quote or a newline")
  else "\($n)='\(.Value)'" end
JQ
)"
readonly JQ_ENV_LINES

log "reading $OPENNOVA_SSM_PREFIX/secret/* and tls/*"
tmp="$(mktemp "$RUN/secret.env.XXXXXX")"
for path in secret tls; do
  aws ssm get-parameters-by-path --path "$OPENNOVA_SSM_PREFIX/$path/" --recursive --with-decryption \
    --output json > "$RUN/params.$path.json" \
    || die "cannot read $OPENNOVA_SSM_PREFIX/$path/"
  jq -r "$JQ_ENV_LINES" "$RUN/params.$path.json" >> "$tmp" \
    || die "a parameter under $OPENNOVA_SSM_PREFIX/$path/ cannot go in an env file"
  rm -f "$RUN/params.$path.json"
done
log "  $(wc -l < "$tmp") secrets"
mv -f "$tmp" "$RUN/secret.env"

# --- 2. the release --------------------------------------------------------------------
release="$(ssm_get "$OPENNOVA_SSM_PREFIX/release")" || die "cannot read $OPENNOVA_SSM_PREFIX/release"
case "$release" in
  none|'')
    log "the deployment has no release yet (./deploy/run.sh app deploy); nothing to converge"
    exit 0 ;;
esac
[[ "$release" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$ ]] || die "the release id '$release' is malformed"
bucket="$(env_value "$RUN/infra.env" OPENNOVA_OPS_BUCKET)"
[ -n "$bucket" ] || die "infra.env names no OPENNOVA_OPS_BUCKET"

dir="$RELEASES/$release"
if [ ! -f "$dir/manifest.json" ]; then
  log "downloading release $release"
  stage="$(mktemp -d "$RELEASES/.incoming.XXXXXX")"
  aws s3 cp "s3://$bucket/releases/$OPENNOVA_DEPLOYMENT/$release.tar.gz" "$stage/bundle.tar.gz" --only-show-errors \
    || { rm -rf "$stage"; die "cannot download s3://$bucket/releases/$OPENNOVA_DEPLOYMENT/$release.tar.gz"; }
  mkdir "$stage/tree"
  tar -xzf "$stage/bundle.tar.gz" -C "$stage/tree" --no-same-owner \
    || { rm -rf "$stage"; die "the bundle of release $release does not unpack"; }
  [ "$(jq -r '.id // empty' "$stage/tree/manifest.json" 2>/dev/null)" = "$release" ] \
    || { rm -rf "$stage"; die "the bundle's manifest names another release"; }
  for f in compose/docker-compose.yml compose/docker-compose.prod.yml env/app.prod.env env/release.env; do
    [ -f "$stage/tree/$f" ] || { rm -rf "$stage"; die "the bundle of release $release lacks $f"; }
  done
  rm -rf "$dir"
  mv "$stage/tree" "$dir"
  rm -rf "$stage"
fi
ln -sfn "$dir" "$BASE/current.new"
mv -Tf "$BASE/current.new" "$BASE/current"

# --- 3. helpers ----------------------------------------------------------------------------
# The RDS CA bundle the host's own database tools (opennova-psql, the
# toolbox's db commands) verify against: the one the server and backup images
# pin, by the same SHA-256 (apps/novaworld_server/Dockerfile says how to bump
# it; bump it here and in service-user-data.sh with it).
readonly RDS_CA_BUNDLE_SHA256=90a6f1a7978a1b1b096dca6c57511ca48d3b8cf052f8e704baf358f34ea2f21b
ca=/etc/opennova/rds-global-bundle.pem
if ! echo "$RDS_CA_BUNDLE_SHA256  $ca" | sha256sum -c - >/dev/null 2>&1; then
  log "fetching the RDS CA bundle"
  curl -fsSL --retry 5 https://truststore.pki.rds.amazonaws.com/global/global-bundle.pem -o "$ca.new" \
    || die "cannot fetch the RDS CA bundle"
  echo "$RDS_CA_BUNDLE_SHA256  $ca.new" | sha256sum -c - >/dev/null \
    || die "the RDS CA bundle is not the pinned one (sha256 $RDS_CA_BUNDLE_SHA256): AWS changed it, bump the pin with the images'"
  chmod 0644 "$ca.new"
  mv -f "$ca.new" "$ca"
fi

install_helper() {
  local tmp
  tmp="$(mktemp)"
  cat > "$tmp"
  install -m 0755 "$tmp" "$SBIN/$1"
  rm -f "$tmp"
}

install_helper opennova-compose <<'HELPER'
#!/bin/bash
# opennova-compose: docker compose for the release this host converged
# (written by opennova-converge; the toolbox's app status, restart and down run
# it over SSM). A clean environment: compose takes its variables from the env
# files alone, later files overriding earlier ones.
set -euo pipefail
cur=/opt/opennova/current
[ -d "$cur" ] || { echo "opennova-compose: no release converged yet" >&2; exit 1; }
for f in /run/opennova/infra.env /run/opennova/secret.env; do
  [ -r "$f" ] || { echo "opennova-compose: no $f (run opennova-converge)" >&2; exit 1; }
done
exec env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root \
  docker compose --project-name opennova --project-directory "$cur/compose" \
    -f "$cur/compose/docker-compose.yml" -f "$cur/compose/docker-compose.prod.yml" \
    --env-file "$cur/env/app.prod.env" --env-file /run/opennova/infra.env \
    --env-file "$cur/env/release.env" --env-file /run/opennova/secret.env "$@"
HELPER

install_helper opennova-psql <<'HELPER'
#!/bin/bash
# opennova-psql: psql as the service's own database role (DATABASE_URL from
# infra.env, DATABASE_PASSWORD from secret.env), in a one-off postgres:17
# container on the host's network (written by opennova-converge; the toolbox's
# `db psql` runs it). Arguments go to psql; a terminal gets an interactive one.
set -euo pipefail
value() { sed -n "s/^$2='\\(.*\\)'\$/\\1/p" "$1" | tail -n 1; }
url="$(value /run/opennova/infra.env DATABASE_URL)"
[ -n "$url" ] || { echo "opennova-psql: infra.env names no DATABASE_URL (run opennova-converge)" >&2; exit 1; }
PGPASSWORD="$(value /run/opennova/secret.env DATABASE_PASSWORD)"
export PGPASSWORD
tty=()
if [ -t 0 ] && [ -t 1 ]; then tty=(-t); fi
exec docker run --rm -i "${tty[@]}" --network host -e PGPASSWORD \
  -v /etc/opennova/rds-global-bundle.pem:/app/rds-global-bundle.pem:ro \
  postgres:17-alpine psql "$url" "$@"
HELPER

install_helper opennova-admin-api <<'HELPER'
#!/bin/bash
# opennova-admin-api METHOD PATH [JSON]: the NovaWorld service's admin API from
# the host's loopback (a proxy it trusts, ONNET_TRUSTED_PROXIES), with the admin
# token from secret.env in a header file, never an argument (written by
# opennova-converge; the toolbox's `fleet` commands run it). Prints the reply
# and its status; fails on a status of 400 or more.
set -euo pipefail
method="${1:?usage: opennova-admin-api METHOD PATH [JSON]}"
path="${2:?usage: opennova-admin-api METHOD PATH [JSON]}"
body="${3:-}"
[[ "$method" =~ ^(GET|POST|PUT|DELETE)$ ]] || { echo "opennova-admin-api: no method $method" >&2; exit 2; }
[[ "$path" =~ ^/api/admin/[A-Za-z0-9/_.?=\&-]*$ ]] || { echo "opennova-admin-api: no admin path $path" >&2; exit 2; }
token="$(sed -n "s/^ADMIN_API_TOKEN='\\(.*\\)'\$/\\1/p" /run/opennova/secret.env | tail -n 1)"
[ -n "$token" ] || { echo "opennova-admin-api: secret.env holds no ADMIN_API_TOKEN (secrets sync, then converge)" >&2; exit 1; }
umask 077
auth="$(mktemp /run/opennova/admin-auth.XXXXXX)"
trap 'rm -f "$auth"' EXIT
printf 'Authorization: Bearer %s\n' "$token" > "$auth"
unset token
args=(-sS --max-time 30 -X "$method" -H "@$auth" -H 'Accept: application/json' -w '\nHTTP %{http_code}\n')
if [ -n "$body" ]; then args+=(-H 'Content-Type: application/json' --data-binary "$body"); fi
out="$(curl "${args[@]}" "http://127.0.0.1:8080$path")"
printf '%s\n' "$out"
code="$(printf '%s\n' "$out" | tail -n 1 | sed -n 's/^HTTP \([0-9]*\)$/\1/p')"
[ -n "$code" ] && [ "$code" -lt 400 ]
HELPER

# --- 4. pull and start ----------------------------------------------------------------
log "pulling release $release's images"
for attempt in 1 2 3; do
  if opennova-compose pull --quiet --ignore-buildable; then break; fi
  [ "$attempt" -lt 3 ] || die "the pull failed; nothing was changed"
  sleep 10
done

before="$(opennova-compose ps -q novaworld 2>/dev/null | head -n 1 || true)"
log "starting release $release (compose recreates what changed)"
if ! opennova-compose up -d --remove-orphans --build --wait --wait-timeout 300; then
  opennova-compose ps -a || true
  die "release $release did not come up healthy within 5 minutes; ./deploy/run.sh app logs novaworld shows why, and app rollback converges the previous release"
fi

# --- 5. report -----------------------------------------------------------------------------
after="$(opennova-compose ps -q novaworld | head -n 1)"
migrations="$(docker logs "$after" 2>&1 | grep '\[boot\] migrations:' | tail -n 1 || true)"
if [ -n "$before" ] && [ "$before" = "$after" ]; then
  log "novaworld kept running (unchanged); its boot logged: ${migrations:-no [boot] migrations: line}"
else
  log "novaworld started: ${migrations:-no [boot] migrations: line yet}"
fi
opennova-compose ps --format 'table {{.Service}}\t{{.Status}}\t{{.Image}}'
printf '%s\n' "$release" > "$BASE/converged"

# Keep the newest few releases (and the current one); images no container has
# used for 30 days go too.
find "$RELEASES" -mindepth 1 -maxdepth 1 -type d ! -name '.*' -printf '%T@ %f\n' \
  | sort -rn | awk -v keep="$KEEP_RELEASES" 'NR > keep { print $2 }' \
  | while read -r old; do
      [ "$old" = "$release" ] || rm -rf "${RELEASES:?}/$old"
    done
docker image prune -af --filter until=720h >/dev/null 2>&1 || true
log "release $release converged"
