# shellcheck shell=bash
# on-deploy's hosted game servers: the machines (`fleet`). The game data is
# the site's Game data page (ADR 0064), not the toolbox's.
# deploy/bin/on-deploy sources this file before it runs a command: it defines
# functions and their constants alone, and runs nothing itself. The helpers
# every command shares (log, die, the deployment, the vault, AWS) are
# on-deploy's own.

# ---------------------------------------------------------------------------
# Hosted game servers (ADR 0053; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------

# The opennova:deployment tag the deployment's hosted machines carry: infra.env's,
# else the stack's resource name (opennova-<deployment>).
hosted_deployment_tag() {
  local tag
  tag="${INFRA[ONNET_HOSTED_DEPLOYMENT]:-}"
  [ -n "$tag" ] || tag="opennova-$DEP"
  printf '%s' "$tag"
}

# The service's admin API for its fleet (web's admin fleet routes), which the
# toolbox calls in a web task (admin_api, over ECS Exec).
FLEET_API=/api/admin/fleet
# Its view of the machines and of the servers in their slots.
FLEET_VIEW_API=/api/admin/hosted

# A machine's id as the service names it (its row) or EC2 does (i-...),
# checked before it goes into a host script.
machine_id() {
  [[ "$1" =~ ^[A-Za-z0-9_-]{1,64}$ ]] || die "'$1' is no machine id (fleet status lists them)"
  printf '%s' "$1"
}

# The AMI names the machines launch from (infra/aws/fleet_agent.tf's fleet_ami:
# Amazon's ECS-optimized Amazon Linux 2023, x86_64, kernel 6.1), newest last
# by its creation, to find a newer one than the pinned.
FLEET_AMI_PATTERN='al2023-ami-ecs-hvm-*-kernel-6.1-x86_64'

# fleet status: every machine this deployment tagged (the EC2 view, which
# answers while the service is down), the fleet cluster's (ECS: each
# machine's container instance, its agent and its hostd task, and the AMI
# pinned against Amazon's newest), then the service's own view.
fleet_status() {
  [ $# -eq 0 ] || die "unknown: fleet status $1 (the deployment is --deployment's)"
  aws_auth
  infra_env_load
  local tag cluster
  tag="$(hosted_deployment_tag)"
  log "machines tagged opennova:deployment=$tag (EC2):"
  # shellcheck disable=SC2016  # a JMESPath literal, not a shell expansion
  aws ec2 describe-instances \
    --filters "Name=tag:opennova:hosted,Values=1" "Name=tag:opennova:deployment,Values=$tag" \
    --query 'Reservations[].Instances[].{machine:InstanceId,kind:Tags[?Key==`opennova:machine-kind`]|[0].Value,id:Tags[?Key==`opennova:machine-id`]|[0].Value,state:State.Name,type:InstanceType,public:PublicIpAddress,launched:LaunchTime}' \
    --output table
  cluster="$(fleet_cluster)"
  if [ -n "$cluster" ]; then
    fleet_ecs_status "$cluster"
    fleet_ami_status
  else
    log "no fleet cluster (hosting is off)"
  fi
  log "the service's fleet ($FLEET_VIEW_API):"
  admin_api GET "$FLEET_VIEW_API" \
    || err "the service did not answer (app status, app logs web)"
}

# The fleet cluster $1 as ECS has it: the hostd service's counts and rollout,
# then one line per container instance (a machine): its machine id and kind
# (the attributes its user-data registered), its EC2 id, whether its ECS agent
# is connected, its status, its AMI (ecs.ami-id) and its hostd task (the
# revision it runs, its health).
fleet_ecs_status() {
  local cluster="$1" arns=() task_arns=() instances='{"containerInstances":[]}' tasks='{"tasks":[]}'
  log "the fleet cluster $cluster (ECS):"
  aws ecs describe-services --cluster "$cluster" --services hostd --output json \
    | jq -r '.services[0] // empty | "  hostd: \(.desiredCount) desired, \(.runningCount) running, \(.pendingCount) pending; \([.deployments[] | select(.status == "PRIMARY")][0].rolloutState // "?") on \(.taskDefinition | split("/") | last)"' \
    || err "describing the hostd service failed"
  mapfile -t arns < <(aws ecs list-container-instances --cluster "$cluster" --query 'containerInstanceArns[]' --output text | tr '\t' '\n' | sed '/^$/d')
  if [ "${#arns[@]}" -gt 0 ]; then
    instances="$(aws ecs describe-container-instances --cluster "$cluster" --container-instances "${arns[@]}" --output json)" \
      || { err "describing the fleet cluster's container instances failed"; return 0; }
  fi
  mapfile -t task_arns < <(aws ecs list-tasks --cluster "$cluster" --service-name hostd --query 'taskArns[]' --output text 2>/dev/null | tr '\t' '\n' | sed '/^$/d')
  if [ "${#task_arns[@]}" -gt 0 ]; then
    tasks="$(aws ecs describe-tasks --cluster "$cluster" --tasks "${task_arns[@]}" --output json 2>/dev/null || echo '{"tasks":[]}')"
  fi
  # shellcheck disable=SC2016  # jq's variables
  jq -r --argjson tasks "$tasks" '
    def pad($n): tostring | if length < $n then . + (" " * ($n - length)) else . end;
    def attr($n): [.attributes[]? | select(.name == $n) | .value][0] // "?";
    def row: [.[0] | pad(9)] + [.[1] | pad(8)] + [.[2] | pad(21)] + [.[3] | pad(7)] + [.[4] | pad(10)] + [.[5] | pad(23)] + [.[6] | tostring] | join("");
    (["MACHINE", "KIND", "EC2", "AGENT", "STATUS", "AMI", "HOSTD"] | row),
    (.containerInstances | sort_by(attr("machine-id") | tonumber? // 0) | .[]
      | .containerInstanceArn as $ci
      | ([$tasks.tasks[] | select(.containerInstanceArn == $ci)][0]) as $t
      | [attr("machine-id"), attr("kind"), .ec2InstanceId, (if .agentConnected then "yes" else "NO" end), .status, attr("ecs.ami-id"),
         (if $t == null then "none" else "\($t.taskDefinitionArn | split("/") | last) \($t.lastStatus | ascii_downcase) \($t.healthStatus // "UNKNOWN" | ascii_downcase)" end)]
      | row)' <<<"$instances"
}

# The AMI the machines launch from (infra.env OPENNOVA_FLEET_AMI_NAME: the
# var-file's fleet_ami) against the newest of Amazon's ECS-optimized AL2023
# images: a newer one is a bump of fleet_ami (staging first), an infra apply and
# a fleet roll.
fleet_ami_status() {
  local pinned newest
  pinned="${INFRA[OPENNOVA_FLEET_AMI_NAME]:-}"
  newest="$(aws ec2 describe-images --owners amazon \
              --filters "Name=name,Values=$FLEET_AMI_PATTERN" Name=state,Values=available \
              --query 'sort_by(Images, &CreationDate)[-1].Name' --output text 2>/dev/null)" || newest=""
  if [ -z "$newest" ] || [ "$newest" = None ]; then
    log "AMI: pinned ${pinned:-none}; Amazon's newest could not be read"
  elif [ "$newest" = "$pinned" ]; then
    log "AMI: pinned $pinned, Amazon's newest"
  else
    log "AMI: pinned ${pinned:-none}; Amazon's newest is $newest (fleet_ami in infra/aws/$DEP.tfvars, staging first; then infra apply and fleet roll)"
  fi
}

# The running hostd task on machine $2 in the fleet cluster $1, by its ARN:
# its container instance found by the machine-id attribute (a service id) or
# by its EC2 id (i-...).
fleet_hostd_task() {
  local cluster="$1" machine="$2" filter instance task
  if [[ "$machine" =~ ^[1-9][0-9]{0,17}$ ]]; then
    filter="attribute:machine-id == $machine"
  elif [[ "$machine" =~ ^i-[0-9a-f]{8,17}$ ]]; then
    filter="ec2InstanceId == $machine"
  else
    die "'$machine' is no machine id (a number, or an EC2 machine's i-...; fleet status lists them)"
  fi
  instance="$(aws ecs list-container-instances --cluster "$cluster" --filter "$filter" \
                --query 'containerInstanceArns[0]' --output text)" \
    || die "listing the fleet cluster's container instances failed"
  [[ "$instance" == arn:* ]] || die "no container instance of $cluster is machine $machine (fleet status: an ended machine has none)"
  task="$(aws ecs list-tasks --cluster "$cluster" --container-instance "$instance" --service-name hostd \
            --desired-status RUNNING --query 'taskArns[0]' --output text)" \
    || die "listing machine $machine's tasks failed"
  [[ "$task" == arn:* ]] || die "machine $machine runs no hostd task (fleet status)"
  printf '%s' "$task"
}

# fleet shell MACHINE: a root shell in machine MACHINE's hostd container (ECS
# Exec), which sees the docker socket, the slots, the trees and hostd's state
# at the machine's own paths. Never an ECS drain of the instance: that stops
# hostd.
fleet_shell() {
  [ $# -eq 1 ] || die "fleet shell takes one MACHINE: its id (the admin fleet view's, fleet status) or its EC2 i-..."
  aws_auth
  infra_env_load
  local cluster task
  cluster="$(fleet_cluster)"
  [ -n "$cluster" ] || die "no fleet cluster (hosting is off): there is no machine"
  task="$(fleet_hostd_task "$cluster" "$1")"
  log "machine $1: hostd task ${task##*/}"
  aws ecs execute-command --cluster "$cluster" --task "$task" --container hostd --interactive --command /bin/sh
}

# fleet roll [--dry-run] [--yes]: the service's plan (GET .../fleet/roll: each
# machine that takes servers, replaced when it runs an older image than the
# pinned AMI, kept otherwise), then, unless --dry-run, the machines it would
# replace drained (POST, their ids), each one's servers moving to a new machine
# one at a time, no restart counted. For an AMI, launch template or user-data
# change alone: a new hostd reaches the machines with app deploy.
fleet_roll() {
  local dry=0 yes=0 plan ids
  while [ $# -gt 0 ]; do
    case "$1" in
      --dry-run) dry=1; shift ;;
      --yes) yes=1; shift ;;
      *) die "unknown: fleet roll $1 (try: --dry-run, --yes)" ;;
    esac
  done
  aws_auth
  plan="$(admin_api_json GET "$FLEET_API/roll")" || die "the service refused or did not answer (app logs web)"
  log "the roll's plan (the pinned image: $(jq -r '.image_id // "none"' <<<"$plan")):"
  jq -r '
    def pad($n): tostring | if length < $n then . + (" " * ($n - length)) else . end;
    def row: [.[0] | pad(9)] + [.[1] | pad(8)] + [.[2] | pad(10)] + [.[3] | pad(23)] + [.[4] | pad(9)] + [.[5] | pad(9)] + [.[6] | tostring] | join("");
    (["MACHINE", "KIND", "STATE", "IMAGE", "SERVERS", "ACTION", "WHY"] | row),
    (.machines[] | [.id, .kind, .state, (.image_id // "?"), .servers, .action, .why] | row)' <<<"$plan"
  ids="$(jq -c '.replace // []' <<<"$plan")"
  if [ "$ids" = "[]" ]; then
    log "no machine to replace"
    return 0
  fi
  if [ "$dry" -eq 1 ]; then
    log "--dry-run: nothing drains (machines $(jq -r 'join(" ")' <<<"$ids") would)"
    return 0
  fi
  confirm_prod "replace machines $(jq -r 'join(", ")' <<<"$ids") (their servers move to new machines, one at a time per machine)" "$yes"
  admin_api POST "$FLEET_API/roll" "$(jq -nc --argjson m "$ids" '{machines: $m}')" \
    || die "the service refused or did not answer (app logs web)"
}

# fleet logs MACHINE [SLOT|hostd] [--since 1h] [--follow]: a machine's logs,
# read from CloudWatch alone (the machines' log group, infra.env's
# OPENNOVA_MACHINES_LOG_GROUP), so no shell on the machine and no SSM: slot k's
# container output (stream <machine id>/slot-<k>, each server on the slot in
# turn), hostd's (its running task's stream, hostd/hostd/<task id>, found in
# the fleet cluster while the machine runs; `app logs hostd` reads every
# machine's, an ended one's among them), or with neither every slot stream of
# the machine, each line naming its stream. MACHINE is the service's id (the
# admin fleet view's "Machine N, slot k", fleet status's id column), or the
# EC2 machine's (i-...), whose opennova:machine-id tag names it while EC2 still
# lists it. The streams outlive the machine (the group keeps 30 days).
fleet_logs() {
  local machine="" slot="" since=1h follow=() streams=() format=short id group
  while [ $# -gt 0 ]; do
    case "$1" in
      --follow|-f) follow=(--follow); shift ;;
      --since) [ $# -ge 2 ] || die "--since needs a value (10m, 2h, 1d)"; since="$2"; shift 2 ;;
      -*) die "unknown: fleet logs $1 (try: MACHINE [SLOT|hostd] [--since 1h] [--follow])" ;;
      *)
        if [ -z "$machine" ]; then machine="$1"
        elif [ -z "$slot" ]; then slot="$1"
        else die "fleet logs takes one MACHINE and one SLOT (or hostd): $1"
        fi
        shift ;;
    esac
  done
  [ -n "$machine" ] || die "fleet logs needs a MACHINE: its id (the admin fleet view's, fleet status)"
  case "$slot" in
    ""|hostd|[0-9]|[12][0-9]|3[01]) ;;
    *) die "'$slot' is no slot (0 to 31) and not hostd" ;;
  esac
  aws_auth
  infra_env_load
  group="$(infra_get OPENNOVA_MACHINES_LOG_GROUP)"
  if [[ "$machine" =~ ^[1-9][0-9]{0,17}$ ]]; then
    id="$machine"
  elif [[ "$machine" =~ ^i-[0-9a-f]{8,17}$ ]]; then
    # shellcheck disable=SC2016  # a JMESPath literal, not a shell expansion
    id="$(aws ec2 describe-instances --instance-ids "$machine" \
            --query 'Reservations[0].Instances[0].Tags[?Key==`opennova:machine-id`]|[0].Value' \
            --output text)" \
      || die "EC2 does not list $machine (an ended machine's logs are read by its id: the admin fleet view's)"
    [[ "$id" =~ ^[1-9][0-9]{0,17}$ ]] || die "$machine carries no opennova:machine-id tag: no machine of the fleet"
    log "$machine is machine $id"
  else
    die "'$machine' is no machine id (a number, or an EC2 machine's i-...; fleet status lists them)"
  fi
  case "$slot" in
    "") streams=(--log-stream-name-prefix "$id/"); format=detailed ;;
    hostd)
      local cluster task
      cluster="$(fleet_cluster)"
      [ -n "$cluster" ] || die "no fleet cluster (hosting is off): there is no machine"
      task="$(fleet_hostd_task "$cluster" "$id")"
      streams=(--log-stream-names "hostd/hostd/${task##*/}") ;;
    *) streams=(--log-stream-names "$id/slot-$slot") ;;
  esac
  aws logs tail "$group" "${streams[@]}" --since "$since" --format "$format" ${follow[@]+"${follow[@]}"}
}

cmd_fleet() {
  local action="${1:-status}"
  shift || true
  case "$action" in
    status) fleet_status "$@" ;;
    logs) fleet_logs "$@" ;;
    shell) fleet_shell "$@" ;;
    roll) fleet_roll "$@" ;;
    drain|end)
      take_yes "$@"
      [ "${#OPT_REST[@]}" -eq 1 ] || die "fleet $action needs one MACHINE (fleet status lists them)"
      local machine
      machine="$(machine_id "${OPT_REST[0]}")" || exit 1
      [ "$action" = drain ] || confirm_prod "end machine $machine (its servers move off first)" "$OPT_YES"
      aws_auth
      admin_api POST "$FLEET_API/machines/$machine/$action" || die "the service refused or did not answer (app logs web)" ;;
    *) die "unknown: fleet $action (try: status|logs|shell|drain|end|roll)" ;;
  esac
}
