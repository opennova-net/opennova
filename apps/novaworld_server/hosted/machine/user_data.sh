#!/bin/bash
# A hosted server's machine (ADR 0053): one opennova-serve, for one hosted
# server, on an EC2 machine of its own. The NovaWorld service's Ec2Provider
# (apps/novaworld_server/hosted/ec2_provider.h) renders this file as the
# machine's user-data at every launch, filling in the two values below, the
# only per-launch input it carries; cloud-init runs it once, as root, at the
# machine's first boot (a stock Ubuntu 24.04 image: infra/aws, "Hosted game
# servers"). It
#
#   1. installs docker (held, so no unattended upgrade restarts it under the
#      server);
#   2. mounts the game data volume (the private snapshot's, labelled
#      opennova-jo) read-only at /opt/jo;
#   3. fetches the server's bootstrap bundle from the service over the private
#      network, once, with the one-time token below: the working directory's
#      files (the secret ones among them), opennova-serve's arguments and the
#      serve image (apps/novaworld_server/hosted/bootstrap.h);
#   4. writes the working directory /srv/serve (0700, the secret files 0600,
#      owned by the image's user 10001) and the run's arguments and image
#      (/etc/opennova);
#   5. pulls the image, fences the server's UID off the instance metadata
#      service (the role's credentials), and starts opennova-serve.service,
#      which runs the server in its container (/usr/local/sbin/opennova-serve-run).
#
# The machine lives as long as that server: whatever ends it (exit 0 for a
# rotation's end or the admin's GOTO MENUSTATE, 3 for the service's
# ServerStopHosting, any other code, a signal), the unit powers the machine
# off, and InstanceInitiatedShutdownBehavior=terminate ends it. Nothing here
# restarts the server: the supervisor alone counts and makes crash restarts
# (on a new machine with new credentials: 3, the count forgiven after an hour
# of running), and it tells a stop it asked for from a crash by its own row,
# never by the exit code (apps/novaworld_server/hosted/supervisor.h). A step
# here that fails powers the machine off the same way, which the supervisor
# reads as a crash. Each step's line and the server's end reach the serial
# console (`aws ec2 get-console-output`), which outlives the machine.
#
# Never `set -x`: the token must reach no console and no log.

set -euo pipefail
umask 077

BOOTSTRAP_URL='@OPENNOVA_BOOTSTRAP_URL@'
BOOTSTRAP_TOKEN='@OPENNOVA_BOOTSTRAP_TOKEN@'

readonly DATA_LABEL=opennova-jo
readonly DATA_DIR=/opt/jo
readonly WORK_DIR=/srv/serve
readonly CONF_DIR=/etc/opennova
readonly RUN_DIR=/run/opennova
readonly SERVE_UID=10001

log() { printf 'opennova-machine: %s\n' "$*"; }

end_machine() {
  log "the bootstrap failed ($1); powering the machine off"
  systemctl poweroff --no-block || poweroff
  exit 1
}
trap 'end_machine "line $LINENO"' ERR

# --- 1. docker -----------------------------------------------------------------
log "installing docker"
export DEBIAN_FRONTEND=noninteractive
apt_get() { apt-get -o DPkg::Lock::Timeout=300 -q -y "$@"; }
for attempt in 1 2 3; do
  if apt_get update && apt_get install --no-install-recommends docker.io iptables; then break; fi
  [ "$attempt" -lt 3 ] || end_machine "apt-get could not install docker"
  sleep 15
done
# An upgrade of these restarts docker, and with it the server's container.
apt-mark hold docker.io containerd runc >/dev/null || log "the docker packages could not be held"
systemctl enable --now docker
# The metadata fence (step 5) matches the server's UID on the host: no user
# namespace remapping may stand between the container's UID and the host's.
if docker info --format '{{json .SecurityOptions}}' | grep -q userns; then
  end_machine "docker remaps user namespaces"
fi

# --- 2. the game data ------------------------------------------------------------
log "mounting the game data"
for _ in $(seq 1 60); do
  [ -e "/dev/disk/by-label/$DATA_LABEL" ] && break
  sleep 2
done
[ -e "/dev/disk/by-label/$DATA_LABEL" ] || end_machine "no volume labelled $DATA_LABEL"
mkdir -p "$DATA_DIR"
mount -t ext4 -o ro,noload,nodev,nosuid,noexec "LABEL=$DATA_LABEL" "$DATA_DIR"
for archive in resource.pff localres.pff language.pff; do
  [ -n "$(find "$DATA_DIR" -maxdepth 1 -type f -iname "$archive" -print -quit)" ] \
    || end_machine "the data volume holds no $archive"
done

# --- 3. the bootstrap bundle -------------------------------------------------------
log "fetching the bootstrap bundle"
install -d -m 0700 "$RUN_DIR"
# The token goes to curl in a file (a builtin writes it): never on a command line.
# It is spent at the first answer: a retry after a lost reply gets 404, the
# machine powers off, and the supervisor counts one crash restart.
printf 'Authorization: Bearer %s\n' "$BOOTSTRAP_TOKEN" > "$RUN_DIR/auth"
unset BOOTSTRAP_TOKEN
curl -fsS --retry 6 --retry-connrefused --retry-delay 5 --connect-timeout 10 --max-time 30 \
  -X POST -H "@$RUN_DIR/auth" -o "$RUN_DIR/bundle.json" "$BOOTSTRAP_URL" \
  || end_machine "the bundle did not come"
rm -f "$RUN_DIR/auth"

# --- 4. the working directory ----------------------------------------------------
log "writing the working directory"
install -d -m 0755 "$CONF_DIR"
install -d -m 0700 -o "$SERVE_UID" -g "$SERVE_UID" "$WORK_DIR"
python3 - "$RUN_DIR/bundle.json" "$WORK_DIR" "$CONF_DIR" "$SERVE_UID" <<'PY'
import base64, json, os, re, sys

bundle_path, work, conf, uid = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
with open(bundle_path, 'rb') as f:
    bundle = json.load(f)
image = bundle.get('image')
if not isinstance(image, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9./:@_-]{0,254}', image):
    sys.exit('the bundle names no usable image')
args = bundle.get('args')
if not isinstance(args, list) or not args or not all(isinstance(a, str) and a and '\0' not in a for a in args):
    sys.exit('the bundle carries no usable arguments')
files = bundle.get('files')
if not isinstance(files, list) or not files:
    sys.exit('the bundle carries no files')
for entry in files:
    name = entry.get('name') if isinstance(entry, dict) else None
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9._-]{1,64}', name) or name in ('.', '..'):
        sys.exit('the bundle names a file that is no bare name')
    data = base64.b64decode(entry.get('content_base64', ''), validate=True)
    # Owner-only unless the service says the file holds no secret.
    mode = 0o644 if entry.get('secret') is False else 0o600
    fd = os.open(os.path.join(work, name), os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, mode)
    os.fchmod(fd, mode)
    os.fchown(fd, uid, uid)
    with os.fdopen(fd, 'wb') as f:
        f.write(data)
with open(os.path.join(conf, 'serve.image'), 'w') as f:
    f.write(image + '\n')
with open(os.path.join(conf, 'serve.args'), 'wb') as f:
    f.write(b''.join(a.encode('utf-8') + b'\0' for a in args))
for name in ('serve.image', 'serve.args'):
    os.chmod(os.path.join(conf, name), 0o644)
print('opennova-machine: server %s, %d files' % (bundle.get('server_id'), len(files)))
PY
rm -f "$RUN_DIR/bundle.json"

# --- 5. the server -------------------------------------------------------------
image="$(cat "$CONF_DIR/serve.image")"
log "pulling $image"
for attempt in 1 2 3; do
  if docker pull -q "$image" >/dev/null; then break; fi
  [ "$attempt" -lt 3 ] || end_machine "the image did not pull"
  sleep 15
done

cat > /usr/local/sbin/opennova-serve-run <<'RUN'
#!/bin/bash
# opennova-serve in its container (opennova-serve.service's ExecStart): the game
# data read-only at /opt/jo, the working directory at /srv/serve, the machine's
# own network (NovaWorld takes the server's address from its session's
# datagrams), and the arguments and the image the bootstrap bundle named. It
# runs as UID 10001 with no capability and no way to gain one (no setuid), so
# it never leaves the UID the metadata fence matches; it binds only ports above
# 1024 and needs none. Its output goes to the journal (journalctl -u
# opennova-serve, over SSM).
set -euo pipefail
image="$(cat /etc/opennova/serve.image)"
mapfile -d '' -t args < /etc/opennova/serve.args
exec /usr/bin/docker run --rm --name opennova-serve --network host --user 10001:10001 \
  --cap-drop ALL --security-opt no-new-privileges \
  --stop-timeout 60 --log-driver journald \
  --mount type=bind,source=/opt/jo,target=/opt/jo,readonly \
  --mount type=bind,source=/srv/serve,target=/srv/serve \
  "$image" "${args[@]}"
RUN
cat > /usr/local/sbin/opennova-serve-fence <<'FENCE'
#!/bin/sh
# opennova-serve.service's ExecStartPre, at every start: the server runs as UID
# 10001 on the machine's own network, which reaches the instance metadata
# service and with it the machine role's credentials. These rules refuse that
# UID every packet to the metadata addresses (IPv4, and Nitro's IPv6 where the
# kernel has IPv6 at all).
set -eu
iptables -C OUTPUT -d 169.254.169.254 -m owner --uid-owner 10001 -j REJECT 2>/dev/null \
  || iptables -I OUTPUT -d 169.254.169.254 -m owner --uid-owner 10001 -j REJECT
if [ -e /proc/net/if_inet6 ]; then
  ip6tables -C OUTPUT -d fd00:ec2::254 -m owner --uid-owner 10001 -j REJECT 2>/dev/null \
    || ip6tables -I OUTPUT -d fd00:ec2::254 -m owner --uid-owner 10001 -j REJECT
fi
FENCE
cat > /usr/local/sbin/opennova-serve-ended <<'ENDED'
#!/bin/sh
# opennova-serve.service's ExecStopPost: the server's end on the serial console,
# which `aws ec2 get-console-output` still reads once the machine is gone.
echo "opennova-machine: opennova-serve ended (${SERVICE_RESULT:-unknown}, ${EXIT_CODE:-unknown} ${EXIT_STATUS:-}); the machine powers off" > /dev/console
ENDED
chmod 0755 /usr/local/sbin/opennova-serve-run /usr/local/sbin/opennova-serve-fence \
  /usr/local/sbin/opennova-serve-ended

cat > /etc/systemd/system/opennova-serve.service <<'UNIT'
[Unit]
Description=opennova-serve, the hosted server this machine runs (ADR 0053)
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target
# Whatever ends the server ends the machine (its shutdown behaviour is
# terminate). No restart here: the supervisor restarts a crashed server on a
# new machine, and counts it.
SuccessAction=poweroff
FailureAction=poweroff

[Service]
Type=exec
ExecStartPre=/usr/local/sbin/opennova-serve-fence
ExecStartPre=-/usr/bin/docker rm -f opennova-serve
ExecStart=/usr/local/sbin/opennova-serve-run
ExecStopPost=/usr/local/sbin/opennova-serve-ended
Restart=no
TimeoutStartSec=120
TimeoutStopSec=90

[Install]
WantedBy=multi-user.target
UNIT
chmod 0644 /etc/systemd/system/opennova-serve.service
# The fence holds before the server ever starts: UID 10001's connection to the
# metadata service is refused (curl's exit 7), neither answered nor timed out.
/usr/local/sbin/opennova-serve-fence || end_machine "the metadata fence did not apply"
imds_rc=0
setpriv --reuid="$SERVE_UID" --regid="$SERVE_UID" --clear-groups \
  curl -s -m 3 -o /dev/null http://169.254.169.254/latest/meta-data/ || imds_rc=$?
[ "$imds_rc" -eq 7 ] || end_machine "UID $SERVE_UID still reaches the metadata service (curl exit $imds_rc)"
systemctl daemon-reload
systemctl enable opennova-serve.service
systemctl start --no-block opennova-serve.service
log "opennova-serve.service started; the machine ends with it"
