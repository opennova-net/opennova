#!/bin/bash
# A machine of the hosted servers' fleet: a shared machine running up to its
# slots' worth of opennova-serve containers, or an own machine running one.
# The NovaWorld service's Ec2MachineDriver
# (apps/novaworld_server/hosted/ec2_machine_driver.h) renders this file as the
# machine's user-data at its launch, filling in the values below, the only
# per-machine input it carries; cloud-init runs it once, as root, at the
# machine's first boot (a stock Ubuntu 24.04 image from the launch template of
# the machine's kind: infra/aws). It
#
#   1. writes the one-time bootstrap token for the agent (0600), and nothing
#      else of it anywhere;
#   2. reserves the slots' ports, 32768-32831 (hosted/slots.h: the game's UDP
#      32768 + k, the remote admin's TCP 32800 + k), out of Linux's ephemeral
#      range, so no outgoing socket ever holds one;
#   3. installs docker (held, so no unattended upgrade restarts it under the
#      servers), with no user-namespace remapping: a slot's UID in its
#      container is the machine's, which the fences and the agent's egress
#      counters match;
#   4. mounts every data set's volume by its label alone (an ext4 file system
#      deploy/hosted-data labels by Ec2MachineDriver::volume_label, od-jop2 for
#      jop_2_consumer), waiting for each label to come up, read-only at
#      /opt/game/<data set>, where the agent binds it into that data set's
#      containers, and again at every boot;
#   5. fences the slots' users (UIDs 10001-10032) off the instance metadata
#      service (the machine role's credentials, this user-data with its token)
#      and off the service's internal port (the agents' routes: only the
#      agent, root, may reach it), checks both, and keeps both at every start
#      of the agent;
#   6. pulls the serve image (the containers' first start pulls nothing),
#      copies opennova-hostd out of the hostd image to /usr/local/bin and
#      starts it under systemd (Restart=always): it redeems the token over the
#      service's internal listener, then runs what the service places on the
#      machine's slots (apps/hostd/README.md).
#
# The machine lives until the service ends it. A step here that fails powers
# the machine off (InstanceInitiatedShutdownBehavior=terminate ends it), and
# the service, which never hears from its agent, ends the row. Each step's
# line reaches the serial console (`aws ec2 get-console-output`).
#
# Never `set -x`: the token must reach no console and no log.

set -euo pipefail
umask 077

SERVICE_ADDRESS='@OPENNOVA_SERVICE_ADDRESS@'
SERVICE_PORT='@OPENNOVA_SERVICE_PORT@'
MACHINE_ID='@OPENNOVA_MACHINE_ID@'
MACHINE_TOKEN='@OPENNOVA_MACHINE_TOKEN@'
SERVE_IMAGE='@OPENNOVA_SERVE_IMAGE@'
HOSTD_IMAGE='@OPENNOVA_HOSTD_IMAGE@'
# "<data set>=<volume label> ...", in the data sets' sorted order.
DATA_SETS='@OPENNOVA_DATA_SETS@'

readonly GAME_ROOT=/opt/game
readonly CONF_DIR=/etc/opennova
readonly TOKEN_FILE=/etc/opennova/hostd.token
readonly HOSTD=/usr/local/bin/opennova-hostd
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

# --- 3. docker -------------------------------------------------------------------
log "installing docker"
export DEBIAN_FRONTEND=noninteractive
apt_get() { apt-get -o DPkg::Lock::Timeout=300 -q -y "$@"; }
for attempt in 1 2 3; do
  if apt_get update && apt_get install --no-install-recommends docker.io iptables curl; then break; fi
  [ "$attempt" -lt 3 ] || end_machine "apt-get could not install docker"
  sleep 15
done
# An upgrade of these restarts docker, and with it every slot's container.
apt-mark hold docker.io containerd runc >/dev/null || log "the docker packages could not be held"
systemctl enable --now docker
if docker info --format '{{json .SecurityOptions}}' | grep -q userns; then
  end_machine "docker remaps user namespaces"
fi

# --- 4. the data sets ------------------------------------------------------------
# Each volume by its label: the launch template's volumes come up in their own
# time, so each label is waited for (two minutes at most).
for entry in $DATA_SETS; do
  data_set="${entry%%=*}"
  label="${entry#*=}"
  for _ in $(seq 1 60); do
    [ -e "/dev/disk/by-label/$label" ] && break
    udevadm settle || true
    sleep 2
  done
  [ -e "/dev/disk/by-label/$label" ] || end_machine "no volume labelled $label for data set $data_set"
  source="LABEL=$label"
  log "mounting data set $data_set ($source)"
  mkdir -p "$GAME_ROOT/$data_set"
  printf '%s %s ext4 ro,noload,nodev,nosuid,noexec,nofail 0 0\n' "$source" "$GAME_ROOT/$data_set" >> /etc/fstab
  mount "$GAME_ROOT/$data_set"
  # The boot's three archives, which every data set ships, and Escalation's
  # version.txt, which Joint Operations' alone does (no other data set's
  # volume passes for its).
  for archive in resource.pff localres.pff language.pff; do
    [ -n "$(find "$GAME_ROOT/$data_set" -maxdepth 1 -type f -iname "$archive" -print -quit)" ] \
      || end_machine "data set $data_set holds no $archive"
  done
  if [ "$data_set" = jop_2_consumer ]; then
    [ -n "$(find "$GAME_ROOT/$data_set" -maxdepth 3 -type f -ipath '*/expansion/jox01/version.txt' -print -quit)" ] \
      || end_machine "data set $data_set holds no expansion/jox01/version.txt"
  fi
done

# --- 5. the fences ---------------------------------------------------------------
printf 'SERVICE_ADDRESS=%s\nSERVICE_PORT=%s\n' "$SERVICE_ADDRESS" "$SERVICE_PORT" > "$CONF_DIR/fence.conf"
chmod 0644 "$CONF_DIR/fence.conf"
cat > /usr/local/sbin/opennova-machine-fence <<'FENCE'
#!/bin/sh
# opennova-hostd.service's ExecStartPre, at every start of the agent: the
# slots' containers run as UIDs 10001-10032 on the machine's own network, which
# reaches the instance metadata service (the machine role's credentials, the
# user-data) and the service's internal port (the agents' routes). These rules
# refuse those UIDs every packet to both, where missing (IPv4, and Nitro's IPv6
# metadata address where the kernel has IPv6 at all).
set -eu
. /etc/opennova/fence.conf
fence() {
  iptables -w 5 -C OUTPUT "$@" -m owner --uid-owner 10001-10032 -j REJECT 2>/dev/null \
    || iptables -w 5 -I OUTPUT "$@" -m owner --uid-owner 10001-10032 -j REJECT
}
fence -d 169.254.169.254
fence -p tcp -d "$SERVICE_ADDRESS" --dport "$SERVICE_PORT"
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
  for url in "http://169.254.169.254/latest/meta-data/" "http://$SERVICE_ADDRESS:$SERVICE_PORT/"; do
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
ExecStart=$HOSTD --service http://$SERVICE_ADDRESS:$SERVICE_PORT --machine $MACHINE_ID
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
chmod 0644 /etc/systemd/system/opennova-hostd.service
systemctl daemon-reload
systemctl enable --now opennova-hostd.service
log "machine $MACHINE_ID: opennova-hostd started"
