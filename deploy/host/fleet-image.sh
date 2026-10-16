#!/bin/bash
# The fleet image's builder: the user data of the one-off instance
# `./deploy/run.sh fleet bake-image` launches (deploy/bin/on-deploy), from
# Canonical's newest Ubuntu 24.04, in a public subnet with the machines'
# security group and no instance role. It does, once, the install stage every
# machine's first boot does (apps/novaworld_common/hosted/machine/
# machine_user_data.sh, step 3: docker, held, with iptables and curl), brings
# the image's packages up to date and pulls the release's serve and hostd
# images (the values below), then leaves nothing of itself behind (cloud-init's
# state and logs, the machine id, apt's lists) and powers off. The toolbox makes
# the image of the stopped instance, points the deployment's fleet image
# parameter at it, and ends the builder. A machine launched from the image still
# runs its whole first-boot script: the install stage finds docker there, and
# the pulls find the layers.
#
# A step that fails leaves the builder running, never powered off, so the
# toolbox's wait for a stopped instance ends in its timeout and it ends the
# builder and makes no image (`aws ec2 get-console-output` says why).

set -euo pipefail
umask 022

# The release's images to pull, space-separated (empty: none).
PULL_IMAGES='@OPENNOVA_PULL_IMAGES@'

log() { printf 'opennova-fleet-image: %s\n' "$*"; }
trap 'log "FAILED at line $LINENO; no image is made of this instance"' ERR

export DEBIAN_FRONTEND=noninteractive
apt_get() {
  apt-get -o DPkg::Lock::Timeout=300 -o Dpkg::Options::=--force-confdef \
    -o Dpkg::Options::=--force-confold -q -y "$@"
}

log "updating the image's packages"
for attempt in 1 2 3; do
  if apt_get update && apt_get upgrade; then break; fi
  [ "$attempt" -lt 3 ] || { log "apt-get upgrade failed"; exit 1; }
  sleep 15
done

# The machines' install stage (machine_user_data.sh step 3), the same packages.
log "installing docker"
for attempt in 1 2 3; do
  if apt_get install --no-install-recommends docker.io iptables curl; then break; fi
  [ "$attempt" -lt 3 ] || { log "docker did not install"; exit 1; }
  sleep 15
done
apt-mark hold docker.io containerd runc >/dev/null
systemctl enable --now docker

for image in $PULL_IMAGES; do
  log "pulling $image"
  for attempt in 1 2 3; do
    if docker pull -q "$image" >/dev/null; then break; fi
    [ "$attempt" -lt 3 ] || { log "$image did not pull"; exit 1; }
    sleep 15
  done
done

log "cleaning up"
apt-get clean
rm -rf /var/lib/apt/lists/*
cloud-init clean --logs --machine-id
sync
log "baked; powering off"
systemctl poweroff
