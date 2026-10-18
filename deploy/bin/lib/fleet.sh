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
# What needs an admin: the fleet's health and the firing alarms.
ATTENTION_API=/api/admin/attention
# A page of one log, and the stream it names (the logs route's).
LOGS_API=/api/admin/logs

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

# The attention list's JSON ($ATTENTION_API) on stdin as lines: each finding,
# danger first, with its level, subject and since when, then the alarms that
# fire (or why none are read).
fleet_attention_lines() {
  jq -r '
    def since($t): if $t then " (since \($t))" else "" end;
    (if (.findings | length) == 0 then "  nothing needs you: every machine reports and every server is running or starting on time"
     else (.findings[] | "  \(.level | ascii_upcase) \(.subject): \(.text)" + since(.since)) end),
    (if (.alarms_read | not) then "  alarms: not read here (a failed read is a finding above)"
     elif (.alarms | length) == 0 then "  alarms: none fires"
     else (.alarms[] | "  ALARM \(.name)" + since(.state_since) + ": \(.description)") end)'
}

# The fleet view's JSON ($FLEET_VIEW_API) on stdin as one line per machine:
# its id, kind, state and slots, its address, its last report, its use, its
# agent, and what holds it or is out of date.
fleet_machine_lines() {
  jq -r '
    def gib: ((. / 1073741824 * 10) | floor) / 10 | tostring;
    if (.machines | length) == 0 then "  no machine" else
    .machines[] | "  machine \(.id) (\(.kind)) \(.state), \(.slots_used) of \(.slots) slots"
      + (if .public_address then ", \(.public_address)" else "" end)
      + (if .address then " (static address \(.address.name))" else "" end)
      + ", reported \(.last_report_at // "never")"
      + (if .stats then "; CPU \(.stats.cpu_percent | floor) %, memory \(.stats.memory_used_bytes | gib) of \(.stats.memory_total_bytes | gib) GiB, disk "
           + (if .stats.disk_total_bytes > 0 then "\(.stats.disk_free_bytes * 100 / .stats.disk_total_bytes | floor) % free" else "unknown" end)
         else "; use not reported" end)
      + "; hostd \(.agent_version // "none")"
      + (if .outdated == true then ", older than the release" else "" end)
      + (if .protocol_mismatch_since then ", held: another protocol since \(.protocol_mismatch_since)" else "" end)
      + (if .lost_held then ", held lost by the brake" else "" end)
      + (if .end_reason then " (\(.end_reason))" else "" end)
    end'
}

# fleet status: every machine this deployment tagged (the EC2 view, which
# answers while the service is down), the fleet cluster's (ECS: each
# machine's container instance, its agent and its hostd task, and the AMI
# pinned against Amazon's newest), then the service's own: what needs an
# admin (its attention list) and one line per machine.
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
  local json
  log "what needs an admin (the service's $ATTENTION_API):"
  if json="$(admin_api_json GET "$ATTENTION_API")"; then
    fleet_attention_lines <<<"$json"
  else
    err "the service did not answer (app status, app logs web)"
    return 0
  fi
  log "the service's machines ($FLEET_VIEW_API):"
  if json="$(admin_api_json GET "$FLEET_VIEW_API")"; then
    fleet_machine_lines <<<"$json"
  else
    err "the service did not answer (app status, app logs web)"
  fi
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

# fleet logs server ID [--start N] [--since 1h] [--follow], fleet logs machine
# ID [--since 1h] [--follow]: one log, read from CloudWatch alone (no shell on
# the machine, no SSM). A server's: one start's output (the servers' log group,
# infra.env's OPENNOVA_SERVERS_LOG_GROUP), its newest or start N (the server
# page's Logs tab names them); a machine's: its hostd's (the machines' group,
# OPENNOVA_MACHINES_LOG_GROUP). The stream is the one the service's rows name
# (its logs route, $LOGS_API: a start's row, the machine's agent_log_stream).
# While the service does not answer, a server's streams are read by their
# names (<server id>/g<start>: start N's, or every start's, each line naming
# its stream) and a machine's from its running hostd task (the fleet cluster).
# ID is the service's (the admin pages', fleet status's). The streams outlive
# their server and machine (14 and 30 days).
fleet_logs() {
  local subject="${1:-}" id="${2:-}" start="" since=1h follow=() group path json errors why stream
  case "$subject" in
    server|machine) ;;
    *) die "fleet logs takes server ID or machine ID (try: fleet logs server 34 --since 2h)" ;;
  esac
  [[ "$id" =~ ^[1-9][0-9]{0,17}$ ]] || die "'$id' is no $subject id (the admin pages', fleet status's)"
  shift 2
  while [ $# -gt 0 ]; do
    case "$1" in
      --follow|-f) follow=(--follow); shift ;;
      --since) [ $# -ge 2 ] || die "--since needs a value (10m, 2h, 1d)"; since="$2"; shift 2 ;;
      --start)
        [ "$subject" = server ] || die "--start is a server's: one of its starts"
        if [ $# -lt 2 ] || ! [[ "$2" =~ ^[1-9][0-9]{0,17}$ ]]; then
          die "--start needs a start's number (the server page's Logs tab names them)"
        fi
        start="$2"; shift 2 ;;
      *) die "unknown: fleet logs $subject $id $1 (try: --start N, --since 1h, --follow)" ;;
    esac
  done
  aws_auth
  infra_env_load
  if [ "$subject" = server ]; then
    group="$(infra_get OPENNOVA_SERVERS_LOG_GROUP)"
  else
    group="$(infra_get OPENNOVA_MACHINES_LOG_GROUP)"
  fi
  path="$LOGS_API?subject=$subject&id=$id&limit=1"
  [ -z "$start" ] || path="$path&generation=$start"
  errors="$(mktemp)"
  if json="$(admin_api_json GET "$path" 2>"$errors")"; then
    rm -f "$errors"
    [ "$(jq -r '.source // empty' <<<"$json")" = cloudwatch ] \
      || die "the service reads its logs from $(jq -r '.source // "nowhere"' <<<"$json"), not CloudWatch"
    stream="$(jq -r '.stream // empty' <<<"$json")"
    [ -n "$stream" ] || die "$subject $id has no log yet (a server not yet started, a machine whose hostd never reported)"
    aws logs tail "$group" --log-stream-names "$stream" --since "$since" --format short ${follow[@]+"${follow[@]}"}
    return
  fi
  why="$(admin_api_failure "$errors")"
  rm -f "$errors"
  [[ "$why" != *"HTTP 404"* ]] || die "the service has no $subject $id${start:+ start $start}"
  if [ "$subject" = server ] && [ -n "$start" ]; then
    log "the service did not answer ($why): start $start's stream by its name"
    aws logs tail "$group" --log-stream-names "$id/g$start" --since "$since" --format short ${follow[@]+"${follow[@]}"}
  elif [ "$subject" = server ]; then
    log "the service did not answer ($why): every start's stream of server $id, each line naming its stream"
    aws logs tail "$group" --log-stream-name-prefix "$id/g" --since "$since" --format detailed ${follow[@]+"${follow[@]}"}
  else
    local cluster task
    cluster="$(fleet_cluster)"
    [ -n "$cluster" ] || die "no fleet cluster (hosting is off): there is no machine"
    task="$(fleet_hostd_task "$cluster" "$id")"
    log "the service did not answer ($why): machine $id's running hostd task ${task##*/}"
    aws logs tail "$group" --log-stream-names "hostd/hostd/${task##*/}" --since "$since" --format short ${follow[@]+"${follow[@]}"}
  fi
}

# fleet console MACHINE: the machine's serial console as EC2 has it now
# (--latest): its boot, its user data's and its ECS agent's lines, a kernel's
# panic, where a machine whose hostd never started says why; while EC2 still
# lists it. MACHINE is the service's id (its EC2 machine found by its
# opennova:machine-id tag, among this deployment's) or EC2's i-....
fleet_console() {
  [ $# -eq 1 ] || die "fleet console takes one MACHINE: its id (fleet status lists them) or its EC2 i-..."
  local instance="$1"
  if [[ "$1" =~ ^[1-9][0-9]{0,17}$ ]]; then
    aws_auth
    infra_env_load
    instance="$(aws ec2 describe-instances \
                  --filters "Name=tag:opennova:machine-id,Values=$1" "Name=tag:opennova:deployment,Values=$(hosted_deployment_tag)" \
                  --query 'Reservations[].Instances[].InstanceId | [0]' --output text)" \
      || die "describing machine $1 failed"
    [[ "$instance" =~ ^i-[0-9a-f]{8,17}$ ]] || die "EC2 lists no machine $1 of this deployment (an ended machine's console goes with it)"
  elif [[ "$1" =~ ^i-[0-9a-f]{8,17}$ ]]; then
    aws_auth
  else
    die "'$1' is no machine id (a number, or an EC2 machine's i-...; fleet status lists them)"
  fi
  log "machine $1: $instance's console output (EC2)"
  aws ec2 get-console-output --instance-id "$instance" --latest --output text --query Output
}

cmd_fleet() {
  local action="${1:-status}"
  shift || true
  case "$action" in
    status) fleet_status "$@" ;;
    logs) fleet_logs "$@" ;;
    console) fleet_console "$@" ;;
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
    *) die "unknown: fleet $action (try: status|logs|console|shell|drain|end|roll)" ;;
  esac
}
