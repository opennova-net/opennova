#!/bin/bash
# A machine of the hosted servers' fleet, an ECS container instance of the
# fleet cluster on the ECS-optimized Amazon Linux 2023 image the deployment
# pins (infra/aws/fleet.tf, fleet_agent.tf). The worker's Ec2MachineDriver
# (apps/novaworld_common/hosted/ec2_machine_driver.h) renders this file as the
# machine's user-data, filling in the values below, the only per-machine input
# it carries; cloud-init runs it once, as root, at the first boot, before the
# image's ECS agent starts (ecs.service runs after cloud-final.service). It
# writes the one-time bootstrap token and the machine's id for the agent (each
# 0600 in a 0700 directory, never on a command line), reserves the slots'
# ports (32768-32831: hosted/slots.h's game UDP 32768 + k and admin TCP
# 32800 + k) out of Linux's ephemeral range, and points the ECS agent at the
# fleet cluster with the machine's id and kind as instance attributes. The
# cluster's daemon service then runs opennova-hostd here (apps/hostd), which
# checks its machine id against the attribute, fences the slots' users,
# redeems the token and runs the servers. The ECS agent's cleanup of images it
# did not pull is never switched on here (its default is off): it would delete
# the serve images hostd's containers run, which ECS does not know of. Nor may
# a task run privileged. A step that fails powers the
# machine off (InstanceInitiatedShutdownBehavior=terminate ends it); the
# service, which never hears from its agent, ends the row. Never `set -x`: the
# token must reach no console and no log.

set -euo pipefail
umask 077
trap 'echo "opennova-machine: setup failed at line $LINENO; powering off"; systemctl poweroff --no-block' ERR

CLUSTER='@OPENNOVA_ECS_CLUSTER@'
MACHINE_ID='@OPENNOVA_MACHINE_ID@'
MACHINE_KIND='@OPENNOVA_MACHINE_KIND@'
MACHINE_TOKEN='@OPENNOVA_MACHINE_TOKEN@'
readonly SLOT_PORTS=32768-32831

install -d -m 0700 /etc/opennova
printf '%s\n' "$MACHINE_TOKEN" > /etc/opennova/hostd.token
printf '%s\n' "$MACHINE_ID" > /etc/opennova/machine-id
chmod 0600 /etc/opennova/hostd.token /etc/opennova/machine-id
unset MACHINE_TOKEN

sysctl -w "net.ipv4.ip_local_reserved_ports=$SLOT_PORTS" >/dev/null
printf 'net.ipv4.ip_local_reserved_ports = %s\n' "$SLOT_PORTS" > /etc/sysctl.d/60-opennova-slots.conf
chmod 0644 /etc/sysctl.d/60-opennova-slots.conf

install -d -m 0755 /etc/ecs
cat > /etc/ecs/ecs.config <<CONFIG
ECS_CLUSTER=$CLUSTER
ECS_INSTANCE_ATTRIBUTES={"machine-id": "$MACHINE_ID", "kind": "$MACHINE_KIND"}
ECS_ENABLE_TASK_IAM_ROLE=true
ECS_ENABLE_TASK_IAM_ROLE_NETWORK_HOST=true
ECS_DISABLE_PRIVILEGED=true
CONFIG
chmod 0644 /etc/ecs/ecs.config
echo "opennova-machine: machine $MACHINE_ID ($MACHINE_KIND) joins $CLUSTER"
