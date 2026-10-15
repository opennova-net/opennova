#!/bin/bash
# The NovaWorld service instance's first boot. infra/aws/service.tf renders this
# file as the instance's user data, filling in the @...@ values below (this
# deployment's); cloud-init runs it once, as root, on a stock Ubuntu 24.04
# image. It
#
#   1. installs Docker Engine with its compose and buildx plugins from Docker's
#      apt repository (its signing key checked by fingerprint), jq, iptables
#      and the AWS CLI v2 (a pinned release, checked by SHA-256);
#   2. fences the instance metadata service (the instance role's credentials):
#      only root (the SSM agent, dockerd's awslogs driver, converge) and the
#      NovaWorld server's UID reach it; a container on a bridge network
#      (the portal's nginx, the web build) reaches it neither through the host
#      (hop limit 1) nor through docker's forwarding (DOCKER-USER), nor the
#      service's internal listener;
#   3. writes /etc/opennova/host.env (the deployment, its region, its SSM
#      parameters and converge document);
#   4. installs opennova-converge.service and starts it: it fetches the converge
#      script from the deployment's SSM document and brings the host to the
#      current release, so a new host serves what the old one did with no
#      operator step (deploy/host/converge.sh).
#
# The instance keeps no state of its own: replacing it
# (`./deploy/run.sh infra apply --allow-destroy -replace=aws_instance.service`)
# runs this again on a new one. Each step's line reaches
# /var/log/cloud-init-output.log. This file (a comment included) is the
# instance's user data: a change to it is a new instance (infra/aws/service.tf).

set -euo pipefail
umask 022

readonly DEPLOYMENT='@OPENNOVA_DEPLOYMENT@'
readonly REGION='@AWS_REGION@'
readonly SSM_PREFIX='@OPENNOVA_SSM_PREFIX@'
readonly CONVERGE_DOCUMENT='@OPENNOVA_CONVERGE_DOCUMENT@'
# The service's internal listener (ONNET_INTERNAL_HTTP_BIND: the machines'
# hostd), which no container on the `edge` bridge (deploy/compose/
# docker-compose.prod.yml) may reach either.
readonly INTERNAL_PORT='@OPENNOVA_INTERNAL_PORT@'
readonly EDGE_SUBNET=172.30.0.0/24

# The NovaWorld server's UID (its image's USER): the one non-root user the
# metadata fence admits.
readonly SERVICE_UID=10002
# Docker's apt signing key (https://download.docker.com/linux/ubuntu/gpg).
readonly DOCKER_KEY_FINGERPRINT=9DC858229FC7DD38854AE2D88D81803C0EBFCD88
# The AWS CLI release (the one apps/novaworld_server/Dockerfile pins); a bump
# moves both, with the sum taken from the release AWS signed.
readonly AWSCLI_VERSION=2.36.50
readonly AWSCLI_SHA256=0f15318b3edc4133ad71ce550303babdf2b5dc66bee6b91b4ac8bf05407a78bf

log() { printf 'opennova-host: %s\n' "$*"; }

# --- 1. packages ------------------------------------------------------------------
log "installing packages (deployment $DEPLOYMENT, $REGION)"
export DEBIAN_FRONTEND=noninteractive
apt_get() { apt-get -o DPkg::Lock::Timeout=300 -q -y "$@"; }
for attempt in 1 2 3; do
  if apt_get update && apt_get install --no-install-recommends ca-certificates curl gnupg iptables jq unzip; then break; fi
  [ "$attempt" -lt 3 ] || { log "apt-get failed"; exit 1; }
  sleep 15
done

log "adding Docker's apt repository"
install -m 0755 -d /etc/apt/keyrings
curl -fsSL --retry 5 https://download.docker.com/linux/ubuntu/gpg -o /tmp/docker.asc
fingerprint="$(gpg --show-keys --with-colons /tmp/docker.asc | awk -F: '$1 == "fpr" { print $10; exit }')"
if [ "$fingerprint" != "$DOCKER_KEY_FINGERPRINT" ]; then
  log "Docker's apt key has fingerprint '$fingerprint', not $DOCKER_KEY_FINGERPRINT"
  exit 1
fi
install -m 0644 /tmp/docker.asc /etc/apt/keyrings/docker.asc
rm -f /tmp/docker.asc
# shellcheck disable=SC1091  # the image's own
. /etc/os-release
printf 'deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu %s stable\n' \
  "$VERSION_CODENAME" > /etc/apt/sources.list.d/docker.list

# live-restore keeps the containers running while dockerd itself restarts (an
# upgrade). The services log to CloudWatch (awslogs, each service's own
# setting); the local cache beside it keeps `docker logs` working.
install -d -m 0755 /etc/docker
cat > /etc/docker/daemon.json <<'JSON'
{
  "live-restore": true,
  "log-driver": "local"
}
JSON

log "installing docker"
for attempt in 1 2 3; do
  if apt_get update && apt_get install --no-install-recommends \
      docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin; then break; fi
  [ "$attempt" -lt 3 ] || { log "docker did not install"; exit 1; }
  sleep 15
done
systemctl enable --now docker

log "installing the AWS CLI $AWSCLI_VERSION"
curl -fsSL --retry 5 "https://awscli.amazonaws.com/awscli-exe-linux-x86_64-${AWSCLI_VERSION}.zip" -o /tmp/awscliv2.zip
echo "${AWSCLI_SHA256}  /tmp/awscliv2.zip" | sha256sum -c -
unzip -q -o /tmp/awscliv2.zip -d /tmp
/tmp/aws/install --update --install-dir /usr/local/aws-cli --bin-dir /usr/local/bin
rm -rf /tmp/aws /tmp/awscliv2.zip
aws --version

# --- 2. the metadata fence ----------------------------------------------------------
log "fencing the instance metadata service"
cat > /usr/local/sbin/opennova-fence <<FENCE
#!/bin/bash
# The instance metadata service's fence (deploy/host/service-user-data.sh):
# root and the NovaWorld server (UID $SERVICE_UID) reach it, nothing else on
# the host does, and no container docker forwards for (a bridge network's)
# does. Idempotent; opennova-fence.service runs it at every boot, after docker.
set -euo pipefail
host_rule=(OUTPUT -d 169.254.169.254 -m owner ! --uid-owner 0 -m owner ! --uid-owner $SERVICE_UID -j REJECT)
iptables -C "\${host_rule[@]}" 2>/dev/null || iptables -A "\${host_rule[@]}"
if iptables -n -L DOCKER-USER >/dev/null 2>&1; then
  forward_rule=(DOCKER-USER -d 169.254.169.254 -j REJECT)
  iptables -C "\${forward_rule[@]}" 2>/dev/null || iptables -I "\${forward_rule[@]}"
fi
# The internal listener answers the machines (their security group) alone; a
# container on the edge bridge reaches the host's addresses without passing
# one, so it is refused here.
internal_rule=(INPUT -s $EDGE_SUBNET -p tcp --dport $INTERNAL_PORT -j REJECT)
iptables -C "\${internal_rule[@]}" 2>/dev/null || iptables -I "\${internal_rule[@]}"
FENCE
chmod 0755 /usr/local/sbin/opennova-fence

cat > /etc/systemd/system/opennova-fence.service <<'UNIT'
[Unit]
Description=OpenNova: fence the instance metadata service (root and the NovaWorld server only)
After=docker.service
Wants=docker.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/opennova-fence

[Install]
WantedBy=multi-user.target
UNIT

# --- 3. the host's settings ----------------------------------------------------------
install -d -m 0755 /etc/opennova /opt/opennova /opt/opennova/releases
cat > /etc/opennova/host.env <<ENV
OPENNOVA_DEPLOYMENT='$DEPLOYMENT'
AWS_REGION='$REGION'
AWS_DEFAULT_REGION='$REGION'
OPENNOVA_SSM_PREFIX='$SSM_PREFIX'
OPENNOVA_CONVERGE_DOCUMENT='$CONVERGE_DOCUMENT'
ENV
chmod 0644 /etc/opennova/host.env

# The RDS CA bundle, for the host's own database tools (`db bootstrap`,
# opennova-psql) to verify the database (sslmode=verify-full): the one the
# server and backup images pin, by the same SHA-256 (bump it with theirs and
# converge.sh's). A bundle that does not come, or is not that one, is left
# for converge, which refuses to go on without it.
readonly RDS_CA_BUNDLE_SHA256=90a6f1a7978a1b1b096dca6c57511ca48d3b8cf052f8e704baf358f34ea2f21b
log "fetching the RDS CA bundle"
if curl -fsSL --retry 5 https://truststore.pki.rds.amazonaws.com/global/global-bundle.pem \
    -o /etc/opennova/rds-global-bundle.pem.new \
   && echo "$RDS_CA_BUNDLE_SHA256  /etc/opennova/rds-global-bundle.pem.new" | sha256sum -c - >/dev/null; then
  chmod 0644 /etc/opennova/rds-global-bundle.pem.new
  mv -f /etc/opennova/rds-global-bundle.pem.new /etc/opennova/rds-global-bundle.pem
else
  rm -f /etc/opennova/rds-global-bundle.pem.new
  log "the RDS CA bundle did not come, or is not the pinned one (converge tries again)"
fi

# --- 4. converge -----------------------------------------------------------------------
# At boot the host runs the converge document's current script, the one `app
# deploy` runs (no copy here to fall behind it).
cat > /usr/local/sbin/opennova-converge-boot <<'BOOT'
#!/bin/bash
# Boot-time converge (opennova-converge.service): the deployment's converge
# document's script, fetched now, run as `app deploy` runs it.
set -euo pipefail
set -a
# shellcheck disable=SC1091
. /etc/opennova/host.env
set +a
install -d -m 0700 /run/opennova
script=/run/opennova/converge-boot.sh
for attempt in 1 2 3 4 5 6; do
  if aws ssm get-document --name "$OPENNOVA_CONVERGE_DOCUMENT" --query Content --output text \
      | jq -er '.mainSteps[0].inputs.runCommand | join("\n")' > "$script"; then
    break
  fi
  [ "$attempt" -lt 6 ] || { echo "opennova-converge-boot: the converge document did not come" >&2; exit 1; }
  sleep 10
done
chmod 0700 "$script"
exec /bin/bash "$script"
BOOT
chmod 0755 /usr/local/sbin/opennova-converge-boot

cat > /etc/systemd/system/opennova-converge.service <<'UNIT'
@OPENNOVA_CONVERGE_UNIT@
UNIT

systemctl daemon-reload
systemctl enable --now opennova-fence.service
systemctl enable opennova-converge.service
systemctl start --no-block opennova-converge.service
log "bootstrapped; opennova-converge.service brings the host to its release (journalctl -u opennova-converge)"
