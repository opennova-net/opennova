#!/bin/bash
# A machine of the hosted servers' fleet: a shared machine running up to its
# slots' worth of opennova-serve containers, or an own machine running one.
# The NovaWorld service's Ec2MachineDriver
# (apps/novaworld_common/hosted/ec2_machine_driver.h) renders this file as the
# machine's user-data at its launch, filling in the values below, the only
# per-machine input it carries; cloud-init runs it once, as root, at the
# machine's first boot (an Ubuntu 24.04 image from the launch template of the
# machine's kind: infra/aws, the deployment's fleet image, Canonical's or one
# deploy/host/fleet-image.sh baked). It
#
#   1. writes the one-time bootstrap token for the agent (0600), and nothing
#      else of it anywhere;
#   2. reserves the slots' ports, 32768-32831 (hosted/slots.h: the game's UDP
#      32768 + k, the remote admin's TCP 32800 + k), out of Linux's ephemeral
#      range, so no outgoing socket ever holds one;
#   3. installs docker (held, so no unattended upgrade restarts it under the
#      servers; a baked image has it, held, and skips the install and its
#      apt-get update), with no user-namespace remapping: a slot's UID in its
#      container is the machine's, which the fences and the agent's egress
#      counters match; and the AWS CLI v2 the agent fetches the trees with
#      (the pinned zip, its SHA-256 checked, the same release the service's
#      images carry: apps/novaworld_common/Dockerfile; a baked image has it);
#   4. makes the trees root, /opt/game, where the agent fetches each tree the
#      service names (ADR 0064: one immutable version of a data set's files in
#      the deployment's object store, which the machine role reads) into its
#      own directory and binds it read-only into that tree's containers;
#   5. fences the slots' users (UIDs 10001-10032) off the instance metadata
#      service (the machine role's credentials, this user-data with its token)
#      and off the fleet's internal port on every destination (the
#      agents' routes: only the agent, root, may reach it; the fleet program is
#      reached by a name whose address moves with it, so no rule names an
#      address), checks both, and keeps both at every start of the agent;
#   6. pulls the serve image (the containers' first start pulls nothing),
#      copies opennova-hostd out of the hostd image to /usr/local/bin and
#      starts it under systemd (Restart=always): it redeems the token over the
#      fleet's internal listener (SERVICE_URL, resolved at each request),
#      then fetches the trees the service names and runs what the service
#      places on the machine's slots (apps/hostd/README.md), each container's
#      output going to the machines' CloudWatch log group (LOG_GROUP), stream
#      <machine id>/slot-<k>, through docker's awslogs driver;
#   7. ships the agent's own journal (its lines, its unit's starts and exits)
#      to the same log group, stream <machine id>/hostd, through the same
#      driver (opennova-hostd-log.service). Docker writes with the machine
#      role's credentials; the slots' users never hold them (step 5).
#
# The machine lives until the service ends it. A step here that fails powers
# the machine off (InstanceInitiatedShutdownBehavior=terminate ends it), and
# the service, which never hears from its agent, ends the row. Each step's
# line reaches the serial console (`aws ec2 get-console-output`).
#
# Never `set -x`: the token must reach no console and no log.

set -euo pipefail
umask 077

# The fleet's internal listener, http://<name>:<port>, and that port.
SERVICE_URL='@OPENNOVA_SERVICE_URL@'
SERVICE_PORT='@OPENNOVA_SERVICE_PORT@'
MACHINE_ID='@OPENNOVA_MACHINE_ID@'
MACHINE_TOKEN='@OPENNOVA_MACHINE_TOKEN@'
SERVE_IMAGE='@OPENNOVA_SERVE_IMAGE@'
HOSTD_IMAGE='@OPENNOVA_HOSTD_IMAGE@'
# The machines' CloudWatch log group and its region (the machine's own).
LOG_GROUP='@OPENNOVA_LOG_GROUP@'
REGION='@OPENNOVA_REGION@'

readonly TREES_ROOT=/opt/game
readonly CONF_DIR=/etc/opennova
readonly TOKEN_FILE=/etc/opennova/hostd.token
readonly HOSTD=/usr/local/bin/opennova-hostd
readonly AWS_CLI=/usr/local/bin/aws
# The AWS CLI v2 release the service's images carry (apps/novaworld_common/Dockerfile).
readonly AWSCLI_VERSION=2.36.50
readonly AWSCLI_SHA256=0f15318b3edc4133ad71ce550303babdf2b5dc66bee6b91b4ac8bf05407a78bf
readonly SLOT_UIDS=10001-10032
readonly FIRST_SLOT_UID=10001
readonly LAST_SLOT_UID=10032
readonly SLOT_PORTS=32768-32831

log() { printf 'opennova-machine: %s\n' "$*"; }

end_machine() {
  log "the machine's setup failed ($1); powering the machine off"
  systemctl poweroff --no-block || poweroff
  exit 1
}
trap 'end_machine "line $LINENO"' ERR

# --- 1. the token --------------------------------------------------------------
# A builtin writes it: never on a command line. The agent deletes the file once
# it has redeemed the token.
install -d -m 0755 "$CONF_DIR"
printf '%s\n' "$MACHINE_TOKEN" > "$TOKEN_FILE"
chmod 0600 "$TOKEN_FILE"
unset MACHINE_TOKEN

# --- 2. the slots' ports ---------------------------------------------------------
sysctl -w "net.ipv4.ip_local_reserved_ports=$SLOT_PORTS" >/dev/null
printf 'net.ipv4.ip_local_reserved_ports = %s\n' "$SLOT_PORTS" > /etc/sysctl.d/60-opennova-slots.conf
chmod 0644 /etc/sysctl.d/60-opennova-slots.conf

# --- 3. docker and the AWS CLI ---------------------------------------------------
export DEBIAN_FRONTEND=noninteractive
apt_get() { apt-get -o DPkg::Lock::Timeout=300 -q -y "$@"; }
held="$(apt-mark showhold 2>/dev/null || true)"
if command -v docker >/dev/null && command -v iptables >/dev/null && command -v curl >/dev/null &&
  command -v unzip >/dev/null && grep -qx docker.io <<<"$held"; then
  # The baked fleet image: its packages are there and held, so no apt-get update.
  log "docker is installed and held (a baked image)"
else
  log "installing docker"
  for attempt in 1 2 3; do
    if apt_get update && apt_get install --no-install-recommends docker.io iptables curl unzip; then break; fi
    [ "$attempt" -lt 3 ] || end_machine "apt-get could not install docker"
    sleep 15
  done
  # An upgrade of these restarts docker, and with it every slot's container.
  apt-mark hold docker.io containerd runc >/dev/null || log "the docker packages could not be held"
fi
systemctl enable --now docker
if docker info --format '{{json .SecurityOptions}}' | grep -q userns; then
  end_machine "docker remaps user namespaces"
fi
# The AWS CLI v2 the agent syncs each tree with: the pinned release, checked,
# unless the image has one already.
if command -v aws >/dev/null; then
  log "the AWS CLI is installed ($(aws --version))"
else
  log "installing the AWS CLI v2 $AWSCLI_VERSION"
  for attempt in 1 2 3; do
    if curl -fsSL "https://awscli.amazonaws.com/awscli-exe-linux-x86_64-$AWSCLI_VERSION.zip" -o /tmp/awscliv2.zip; then break; fi
    [ "$attempt" -lt 3 ] || end_machine "the AWS CLI did not download"
    sleep 15
  done
  echo "$AWSCLI_SHA256  /tmp/awscliv2.zip" | sha256sum -c - >/dev/null || end_machine "the AWS CLI zip does not match its checksum"
  unzip -q /tmp/awscliv2.zip -d /tmp
  /tmp/aws/install --install-dir /usr/local/aws-cli --bin-dir /usr/local/bin >/dev/null
  rm -rf /tmp/awscliv2.zip /tmp/aws
  log "the AWS CLI is installed ($("$AWS_CLI" --version))"
fi

# --- 4. the trees root -----------------------------------------------------------
# Where the agent fetches each tree the service names (<root>/<tree id>, a
# .partial directory while it syncs) and binds it into that tree's containers,
# read-only, the base tree at the container's /opt/game and an expansion tree
# at /opt/game/expansion/<code>. Nothing is mounted here: the trees come from
# the object store over the machine role, through the AWS CLI above.
install -d -m 0755 "$TREES_ROOT"

# --- 5. the fences ---------------------------------------------------------------
printf 'SERVICE_PORT=%s\n' "$SERVICE_PORT" > "$CONF_DIR/fence.conf"
chmod 0644 "$CONF_DIR/fence.conf"
cat > /usr/local/sbin/opennova-machine-fence <<'FENCE'
#!/bin/sh
# opennova-hostd.service's ExecStartPre, at every start of the agent: the
# slots' containers run as UIDs 10001-10032 on the machine's own network, which
# reaches the instance metadata service (the machine role's credentials, the
# user-data) and the fleet's internal port (the agents' routes). These
# rules refuse those UIDs every packet to both, where missing (IPv4, and Nitro's
# IPv6 metadata address where the kernel has IPv6 at all): the port on every
# destination, since the fleet's address moves with its instance. They
# also refuse them the slots' remote-admin ports (TCP 32800-32831) on every
# destination: a server's admin.cfg admits the fleet's whole network now
# that its address moves, and the machine's own address is in it, so without
# this a slot could reach a sibling's admin port (its password still guards it).
set -eu
. /etc/opennova/fence.conf
fence() {
  iptables -w 5 -C OUTPUT "$@" -m owner --uid-owner 10001-10032 -j REJECT 2>/dev/null \
    || iptables -w 5 -I OUTPUT "$@" -m owner --uid-owner 10001-10032 -j REJECT
}
fence -d 169.254.169.254
fence -p tcp --dport "$SERVICE_PORT"
fence -p tcp --dport 32800:32831
if [ -e /proc/net/if_inet6 ]; then
  ip6tables -w 5 -C OUTPUT -d fd00:ec2::254 -m owner --uid-owner 10001-10032 -j REJECT 2>/dev/null \
    || ip6tables -w 5 -I OUTPUT -d fd00:ec2::254 -m owner --uid-owner 10001-10032 -j REJECT
fi
FENCE
chmod 0755 /usr/local/sbin/opennova-machine-fence
/usr/local/sbin/opennova-machine-fence || end_machine "the fences did not apply"
# Both fences hold for the first and the last slot's user: their connections are
# refused (curl's exit 7), neither answered nor timed out.
for uid in "$FIRST_SLOT_UID" "$LAST_SLOT_UID"; do
  for url in "http://169.254.169.254/latest/meta-data/" "$SERVICE_URL/"; do
    rc=0
    setpriv --reuid="$uid" --regid="$uid" --clear-groups curl -s -m 3 -o /dev/null "$url" || rc=$?
    [ "$rc" -eq 7 ] || end_machine "UID $uid still reaches $url (curl exit $rc)"
  done
done
log "slot users $SLOT_UIDS are fenced off the metadata service and port $SERVICE_PORT"

# --- 6. the agent ----------------------------------------------------------------
pull() {
  log "pulling $1"
  for attempt in 1 2 3; do
    if docker pull -q "$1" >/dev/null; then return 0; fi
    sleep 15
  done
  end_machine "$1 did not pull"
}
pull "$SERVE_IMAGE"
[ "$HOSTD_IMAGE" = "$SERVE_IMAGE" ] || pull "$HOSTD_IMAGE"
# Created, never started: the copy needs the file system alone.
container="$(docker create --entrypoint /usr/local/bin/opennova-hostd "$HOSTD_IMAGE")"
docker cp "$container:/usr/local/bin/opennova-hostd" "$HOSTD.new"
docker rm "$container" >/dev/null
install -m 0755 -o root -g root "$HOSTD.new" "$HOSTD"
rm -f "$HOSTD.new"
log "opennova-hostd $("$HOSTD" --version)"

cat > /etc/systemd/system/opennova-hostd.service <<UNIT
[Unit]
Description=opennova-hostd, the hosted servers' machine agent
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target

[Service]
ExecStartPre=/usr/local/sbin/opennova-machine-fence
ExecStart=$HOSTD --service $SERVICE_URL --machine $MACHINE_ID --trees-root $TREES_ROOT --aws $AWS_CLI --log-group $LOG_GROUP --log-region $REGION
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
chmod 0644 /etc/systemd/system/opennova-hostd.service
systemctl daemon-reload
systemctl enable --now opennova-hostd.service
log "machine $MACHINE_ID: opennova-hostd started"

# --- 7. the agent's log ------------------------------------------------------------
# journalctl follows the agent's unit from its cursor (this boot's start the
# first time) into a container of the hostd image that copies its input to its
# output, which docker ships like the slots' (non-blocking: CloudWatch out of
# reach drops lines past the buffer and holds nothing up). Logs never end the
# machine: a failure here is logged and the unit tries again every 5 s.
printf "LOG_GROUP='%s'\nREGION='%s'\nMACHINE_ID='%s'\nHOSTD_IMAGE='%s'\n" \
  "$LOG_GROUP" "$REGION" "$MACHINE_ID" "$HOSTD_IMAGE" > "$CONF_DIR/hostd-log.conf"
chmod 0644 "$CONF_DIR/hostd-log.conf"
cat > /usr/local/sbin/opennova-hostd-log <<'RELAY'
#!/bin/sh
set -eu
. /etc/opennova/hostd-log.conf
docker rm -f opennova-hostd-log >/dev/null 2>&1 || true
journalctl --unit=opennova-hostd.service --boot --lines=all --follow --quiet \
    --output=short-iso --cursor-file=/var/lib/opennova-hostd-log/cursor |
  docker run --rm -i --name opennova-hostd-log --network none --read-only --cap-drop ALL \
    --security-opt no-new-privileges --user 65534:65534 --memory 64m --pids-limit 8 \
    --log-driver awslogs --log-opt "awslogs-region=$REGION" --log-opt "awslogs-group=$LOG_GROUP" \
    --log-opt "awslogs-stream=$MACHINE_ID/hostd" --log-opt mode=non-blocking \
    --log-opt max-buffer-size=4m --entrypoint /usr/bin/cat "$HOSTD_IMAGE" >/dev/null
RELAY
chmod 0755 /usr/local/sbin/opennova-hostd-log
cat > /etc/systemd/system/opennova-hostd-log.service <<'UNIT'
[Unit]
Description=opennova-hostd's journal to CloudWatch
Requires=docker.service
After=docker.service

[Service]
ExecStart=/usr/local/sbin/opennova-hostd-log
StateDirectory=opennova-hostd-log
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
chmod 0644 /etc/systemd/system/opennova-hostd-log.service
systemctl daemon-reload
systemctl enable --now opennova-hostd-log.service || log "opennova-hostd-log did not start"
log "machine $MACHINE_ID: logs to $LOG_GROUP ($REGION), streams $MACHINE_ID/hostd and $MACHINE_ID/slot-<k>"
