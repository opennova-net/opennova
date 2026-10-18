# shellcheck shell=bash
# on-deploy's ECS side: the programs' releases (their images pinned by digest,
# their revisions, the services rolled in turn, the release recorded) and
# rollbacks, the services' status, logs, shells and counts, ECS Exec, the
# admin API on a web task and the one-off tasks (`app`).
# deploy/bin/on-deploy sources this file before it runs a command: it defines
# functions and their constants alone, and runs nothing itself. The helpers
# every command shares (log, die, the deployment, the vault, AWS) are
# on-deploy's own.

# ---------------------------------------------------------------------------
# app: the programs on ECS (infra/aws/programs.tf), their releases
# ---------------------------------------------------------------------------

# The service's programs, in the order a deploy updates them: the job
# programs first, then hostd (the machines' agent) before the fleet program it
# reports to, so the fleet never speaks a protocol its agents do not yet, then
# the fleet, web, the lobby (whose restart the players feel) and the gate (the
# players' way in) last.
PROGRAMS=(worker data hostd fleet web lobby gate)

# The programs that are no scaled service of the deployment's cluster: hostd,
# the fleet cluster's DAEMON service (infra/aws/fleet_agent.tf: one task per
# machine, as many as the machines registered). Never a --desired-count, an
# Application Auto Scaling target or a count in `app down`'s record: down and up
# leave it running, and its machines' servers with it.
DAEMON_PROGRAMS=(hostd)

# A program's name, checked.
program_name() {
  case "$1" in
    gate|lobby|web|fleet|worker|data|hostd) printf '%s' "$1" ;;
    *) die "'$1' is no program (gate, lobby, web, fleet, worker, data, hostd)" ;;
  esac
}

# Whether program $1 is a daemon (DAEMON_PROGRAMS).
is_daemon() {
  case " ${DAEMON_PROGRAMS[*]} " in
    *" $1 "*) return 0 ;;
    *) return 1 ;;
  esac
}

# The deployment's cluster (infra.env).
ecs_cluster() {
  infra_env_load
  infra_get OPENNOVA_CLUSTER
}

# The fleet cluster hostd runs in (infra.env), "" with hosting off: no fleet,
# so no hostd.
fleet_cluster() {
  infra_env_load
  printf '%s' "${INFRA[OPENNOVA_FLEET_CLUSTER]:-}"
}

# The cluster program $1's service is in.
program_cluster() {
  if is_daemon "$1"; then fleet_cluster; else ecs_cluster; fi
}

# The programs the deployment runs, in PROGRAMS' order, into RUN_PROGRAMS:
# hostd only with a fleet cluster, which is said once when there is none.
RUN_PROGRAMS=()
DAEMONS_SKIPPED_LOGGED=0
run_programs() {
  local program
  infra_env_load
  RUN_PROGRAMS=()
  for program in "${PROGRAMS[@]}"; do
    if is_daemon "$program" && [ -z "${INFRA[OPENNOVA_FLEET_CLUSTER]:-}" ]; then
      [ "$DAEMONS_SKIPPED_LOGGED" -eq 1 ] || log "$program: skipped, infra.env names no fleet cluster (hosting is off)"
      DAEMONS_SKIPPED_LOGGED=1
      continue
    fi
    RUN_PROGRAMS+=("$program")
  done
}

# RUN_PROGRAMS less the daemons: the deployment's cluster's scaled services.
scaled_programs() {
  local program
  for program in "${RUN_PROGRAMS[@]}"; do
    is_daemon "$program" || printf '%s\n' "$program"
  done
}

# Terraform's task definition of $1 (a program, or backup): the template a
# release's revision is cloned from (infra.env, OPENNOVA_TASK_DEFINITIONS).
task_template() {
  local arn
  infra_env_load
  arn="$(jq -r --arg k "$1" '.[$k] // empty' <<<"$(infra_get OPENNOVA_TASK_DEFINITIONS)")" \
    || die "infra.env's OPENNOVA_TASK_DEFINITIONS is no JSON object (infra apply)"
  [ -n "$arn" ] || die "infra.env names no task definition for $1 (infra apply)"
  printf '%s' "$arn"
}

# The task definition service $1 runs now (its primary deployment's).
service_task_definition() {
  aws ecs describe-services --cluster "$(program_cluster "$1")" --services "$1" \
    --query 'services[0].taskDefinition' --output text \
    || die "describing the $DEP $1 service failed"
}

# One running task of program $1 (its service's first), for ECS Exec.
program_task() {
  local arn
  arn="$(aws ecs list-tasks --cluster "$(ecs_cluster)" --service-name "$1" --desired-status RUNNING \
           --query 'taskArns[0]' --output text)" || die "listing the $DEP $1 tasks failed"
  case "$arn" in
    arn:*) printf '%s' "$arn" ;;
    *) die "no running $DEP $1 task (app status)" ;;
  esac
}

# ---- ECS Exec ------------------------------------------------------------------

# Run the shell script $3 in container $2 of task $1 over ECS Exec (sh, as
# root), its output on stdout. The script travels base64-encoded (ECS Exec
# splits its command on white space), and reads a secret it needs from PID 1's
# environment (ECS hands the container's secrets to its first process; an exec
# session gets none of them), never from an argument. The session plugin ends
# the session when its stdin reaches end of file, cutting the output short
# when the toolbox runs without a terminal: its stdin stays open (a sleep the
# container's end reaps) until the command ends.
ecs_exec() {
  local task="$1" container="$2" script="$3" b64
  b64="$(printf '%s\n' "$script" | base64 -w0)"
  aws ecs execute-command --cluster "$(ecs_cluster)" --task "$task" --container "$container" \
    --interactive --command "sh -c 'echo $b64 | base64 -d | sh'" < <(sleep 3600)
}

# A sh fragment: NAME from PID 1's environment into the shell variable $2.
# Where PID 1 runs as another user (the programs' images: their USER), the
# exec session's root lacks CAP_SYS_PTRACE, which reading another user's
# /proc/<pid>/environ needs, so the read runs as PID 1's own user and group
# (setpriv, util-linux); where it is the session's own (the backup image, root),
# it reads directly.
# shellcheck disable=SC2016  # the task's shell expands it
proc1_env() {
  printf '%s="$(if [ "$(stat -c %%u /proc/1)" = "$(id -u)" ]; then cat /proc/1/environ; else setpriv --reuid="$(stat -c %%u /proc/1)" --regid="$(stat -c %%g /proc/1)" --clear-groups cat /proc/1/environ; fi | tr '"'"'\\000'"'"' '"'"'\\n'"'"' | sed -n '"'"'s/^%s=//p'"'"' | head -n 1)"\n' "$2" "$1"
}

# The service's admin API (apps/novaworld_web: the admin routes) on a web
# task, over ECS Exec, from the task's loopback (a proxy web trusts,
# ONNET_TRUSTED_PROXIES), with the admin token from the container's own
# environment in a header file, never an argument. A POST may carry a JSON body
# ($3) of ids and words alone (it travels in the script). Prints the reply and
# its status; fails on a status of 400 or more, or none.
admin_api() {
  local method="$1" path="$2" body="${3:-}" script out code data=""
  [[ "$method" =~ ^(GET|POST)$ ]] || die "admin_api: no method $method"
  [[ "$path" =~ ^/api/admin/[A-Za-z0-9/_.?=\&-]*$ ]] || die "admin_api: no admin path $path"
  if [ -n "$body" ]; then
    [[ "$body" =~ ^[][{}\":,A-Za-z0-9_\ -]*$ ]] || die "admin_api: a body of more than ids and words"
    data=" -H 'Content-Type: application/json' --data '$body'"
  fi
  infra_env_load
  script="set -eu
$(proc1_env ADMIN_API_TOKEN token)
[ -n \"\$token\" ] || { echo 'the web container holds no ADMIN_API_TOKEN (secrets sync, then app converge)' >&2; exit 1; }
umask 077
auth=\"\$(mktemp)\"
trap 'rm -f \"\$auth\"' EXIT
printf 'Authorization: Bearer %s\n' \"\$token\" > \"\$auth\"
unset token
curl -sS --max-time 30 -X $method -H \"@\$auth\" -H 'Accept: application/json'$data -w '\nHTTP %{http_code}\n' 'http://127.0.0.1:${INFRA[ONNET_HTTP_PORT]:-8080}$path'"
  out="$(ecs_exec "$(program_task web)" web "$script")" || { printf '%s\n' "$out"; return 1; }
  printf '%s\n' "$out"
  code="$(printf '%s\n' "$out" | tr -d '\r' | sed -n 's/^HTTP \([0-9][0-9]*\)$/\1/p' | tail -n 1)"
  [ -n "$code" ] && [ "$code" -lt 400 ]
}

# admin_api's reply body alone, the JSON object (its first line opening one;
# the session's own lines and the status line left out). Fails as admin_api
# does, or when the reply holds no object.
admin_api_json() {
  local out json
  out="$(admin_api "$@")" || { printf '%s\n' "$out" >&2; return 1; }
  json="$(printf '%s\n' "$out" | tr -d '\r' | sed -n '/^{/{p;q;}')"
  [ -n "$json" ] || { err "the admin API's reply to $1 $2 holds no JSON object"; return 1; }
  printf '%s\n' "$json"
}

# Why an admin_api call failed, in one line, from what it wrote to stderr ($1,
# a file): AWS refusing a call ECS Exec into a web task makes (its ListTasks,
# DescribeTasks or ExecuteCommand), the admin API's own status, or the first
# line naming an error.
admin_api_failure() {
  local text op code line
  text="$(tr -d '\r' <"$1" 2>/dev/null || true)"
  op="$(grep -oE -m 1 'AccessDenied[A-Za-z]*\) when calling the [A-Za-z]+ operation' <<<"$text" \
          | sed 's/.* when calling the \([A-Za-z]*\) operation/\1/' || true)"
  if [ -n "$op" ]; then
    printf 'AWS refused the %s call of ECS Exec into a web task' "$op"
    return
  fi
  code="$(sed -n 's/^HTTP \([0-9][0-9]*\)$/\1/p' <<<"$text" | tail -n 1)"
  if [ -n "$code" ]; then
    printf 'the admin API answered HTTP %s' "$code"
    return
  fi
  line="$(grep -m 1 -iE 'error|denied|not authorized|no running|failed|holds no' <<<"$text" || true)"
  printf '%s' "${line:-ECS Exec into a web task failed and said nothing}"
}

# ---- one-off tasks -----------------------------------------------------------------

# The backup task definition the toolbox's one-off tasks run: the current
# release's revision (its image pinned), else Terraform's.
backup_task_definition() {
  local td
  td="$(jq -r '.task_definitions.backup // empty' <<<"$(release_current_manifest)")"
  [ -n "$td" ] || td="$(task_template backup)"
  printf '%s' "$td"
}

# Run the backup task definition once with the command $1 (a JSON array),
# allowing it $2 seconds: a Fargate task in the deployment's public subnets
# and the backup task's security group. Prints its log when it stops and
# succeeds only when its container exited 0. With $3 = exec, it starts with
# ECS Exec on and returns its ARN (in TASK_ARN) as soon as it runs and its
# exec agent answers, for an interactive session; the caller stops it.
TASK_ARN=""
one_off_task() {
  local command="$1" limit="$2" mode="${3:-}" cluster td subnets sg overrides out waited=0 status code reason agent
  infra_env_load
  cluster="$(ecs_cluster)"
  td="$(backup_task_definition)"
  subnets="$(infra_get OPENNOVA_TASK_SUBNETS)"
  sg="$(infra_get OPENNOVA_BACKUP_SECURITY_GROUP)"
  overrides="$(jq -nc --argjson c "$command" '{containerOverrides: [{name: "backup", command: $c}]}')" \
    || die "one_off_task: the command is no JSON array"
  local exec_flag=()
  [ "$mode" != exec ] || exec_flag=(--enable-execute-command)
  out="$(aws ecs run-task --cluster "$cluster" --task-definition "$td" --launch-type FARGATE \
           --platform-version 1.4.0 --started-by opennova-toolbox --propagate-tags TASK_DEFINITION \
           --network-configuration "awsvpcConfiguration={subnets=[$subnets],securityGroups=[$sg],assignPublicIp=ENABLED}" \
           --overrides "$overrides" ${exec_flag[@]+"${exec_flag[@]}"} --output json)" \
    || die "the one-off task did not start (run-task failed)"
  TASK_ARN="$(jq -r '.tasks[0].taskArn // empty' <<<"$out")"
  [ -n "$TASK_ARN" ] || die "the one-off task did not start: $(jq -c '.failures' <<<"$out")"
  log "one-off task ${TASK_ARN##*/} ($td)…"
  while :; do
    out="$(aws ecs describe-tasks --cluster "$cluster" --tasks "$TASK_ARN" --output json 2>/dev/null || echo '{}')"
    status="$(jq -r '.tasks[0].lastStatus // "UNKNOWN"' <<<"$out")"
    if [ "$mode" = exec ] && [ "$status" = RUNNING ]; then
      agent="$(jq -r '[.tasks[0].containers[0].managedAgents[]? | select(.name == "ExecuteCommandAgent") | .lastStatus][0] // ""' <<<"$out")"
      [ "$agent" != RUNNING ] || return 0
    fi
    [ "$status" != STOPPED ] || break
    if [ "$waited" -ge "$limit" ]; then
      aws ecs stop-task --cluster "$cluster" --task "$TASK_ARN" --reason "opennova toolbox: no end after ${waited}s" >/dev/null 2>&1 || true
      task_log "$TASK_ARN"
      die "the one-off task ran past ${limit}s and was stopped"
    fi
    sleep 5
    waited=$((waited + 5))
  done
  [ "$mode" != exec ] || die "the one-off task stopped before its exec agent ran: $(jq -r '.tasks[0].stoppedReason // "?"' <<<"$out")"
  task_log "$TASK_ARN"
  code="$(jq -r '.tasks[0].containers[0].exitCode // "none"' <<<"$out")"
  reason="$(jq -r '.tasks[0].stoppedReason // ""' <<<"$out")"
  if [ "$code" != 0 ]; then
    err "the one-off task ended with exit $code${reason:+ ($reason)}"
    return 1
  fi
}

# A one-off task's log (the backup prefix's stream for it).
task_log() {
  aws logs get-log-events --log-group-name "$(infra_get OPENNOVA_LOG_GROUP)" \
    --log-stream-name "backup/backup/${1##*/}" --start-from-head --output json 2>/dev/null \
    | jq -r '.events[]?.message' || true
}

# ---- releases ---------------------------------------------------------------------

# hostd's image repository (under the registry): its own image
# (apps/serve/Dockerfile's hostd target, serve-image.yml), the fleet
# cluster's daemon.
HOSTD_REPO=opennova-hostd

# Each image a release pins, by its key: the six programs', the portal's
# nginx (web's second container) and the backup task's, the containers of
# the task definitions it registers (named after their keys), and the hosted
# machines' serve image and hostd's (the hostd daemon's revision; its
# container is named hostd too).
SERVICE_IMAGES=(gate lobby web fleet worker data portal backup)
image_repo() {
  case "$1" in
    gate|lobby|web|fleet|worker|data|backup) printf 'opennova-novaworld-%s' "$1" ;;
    portal) printf 'novaworld-web' ;;
    serve) printf 'opennova-serve' ;;
    hostd) printf '%s' "$HOSTD_REPO" ;;
    *) die "'$1' is no image (gate, lobby, web, fleet, worker, data, portal, backup, serve, hostd)" ;;
  esac
}

# <registry>/<repo>:<tag> as <registry>/<repo>@sha256:..., read anonymously
# from the registry (the packages are public).
image_digest() {
  local ref="$1" digest
  digest="$(docker buildx imagetools inspect "$ref" --format '{{json .Manifest}}' 2>/dev/null | jq -r '.digest // empty')"
  case "$digest" in
    sha256:*) printf '%s@%s' "${ref%:*}" "$digest" ;;
    *) return 1 ;;
  esac
}

# The image a release takes for key $1: tag $2 resolved now (or, as
# @sha256:<digest>, that digest as it is, checked to be in the registry), else
# the current release's ($3, its manifest) while it is of the key's
# repository (a release before hostd's own image names the serve image's for
# it), else `latest` resolved now.
pick_image() {
  local key="$1" tag="$2" manifest="$3" kept ref
  if [[ "$tag" == @* ]]; then
    [[ "${tag#@}" =~ ^sha256:[0-9a-f]{64}$ ]] || { err "'$tag' is no digest (@sha256:<64 hex digits>)"; return 1; }
    ref="$(infra_get OPENNOVA_IMAGE_REGISTRY)/$(image_repo "$key")@${tag#@}"
    docker buildx imagetools inspect "$ref" >/dev/null 2>&1 \
      || { err "$ref is not in the registry (is the package public?)"; return 1; }
    printf '%s' "$ref"
    return
  fi
  if [ -z "$tag" ]; then
    kept="$(jq -r --arg k "$key" '.images[$k] // empty' <<<"$manifest")"
    if [ -n "$kept" ] && [ "${kept%@*}" = "$(infra_get OPENNOVA_IMAGE_REGISTRY)/$(image_repo "$key")" ]; then
      printf '%s' "$kept"
      return
    fi
    tag=latest
  fi
  [[ "$tag" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]] || { err "'$tag' is no image tag"; return 1; }
  ref="$(infra_get OPENNOVA_IMAGE_REGISTRY)/$(image_repo "$key"):$tag"
  image_digest "$ref" || { err "cannot resolve $ref to a digest (is the package public, and the tag published?)"; return 1; }
}

# The deployment's release (SSM .../release): its manifest, or {} with none.
# A value that is no manifest with programs (a release of the single service
# instance, before the cutover) reads as none.
release_current_manifest() {
  local value
  value="$(aws ssm get-parameter --name "$SSM_PREFIX/release" --query Parameter.Value --output text 2>/dev/null)" \
    || die "cannot read $SSM_PREFIX/release: run ./deploy/run.sh$DEP_FLAG infra apply first"
  if [ "$value" = none ]; then
    printf '{}'
  elif jq -e 'type == "object" and (.programs | type) == "array"' <<<"$value" >/dev/null 2>&1; then
    printf '%s' "$value"
  else
    printf '{}'
  fi
}

release_manifest() {
  local bucket="$1" id="$2"
  aws s3 cp "s3://$bucket/releases/$id.json" - 2>/dev/null \
    || die "cannot read release $id's manifest (s3://$bucket/releases/$id.json)"
}

# The secrets SSM holds for the deployment (secret/* and tls/*), one full
# parameter name a line: names alone (DescribeParameters), never a value.
ssm_secret_names() {
  local path
  for path in "$SSM_PREFIX/secret" "$SSM_PREFIX/tls"; do
    aws ssm describe-parameters --parameter-filters "Key=Path,Values=$path" \
      --query 'Parameters[].Name' --output text \
      || die "listing $path/ failed"
  done | tr '\t' '\n' | sed '/^$/d'
}

# The registration of a revision of task definition $1 (described, with its
# tags), on stdout: its containers' images from the release ($2, keyed by
# container: hostd's among them), hosting's switch and the serve image for the
# containers that read the fleet's policy (ONNET_HOSTED_PROVIDER: web, the
# fleet, the worker), the hostd image for those that read the machine driver's
# own settings too (ONNET_HOSTED_DEPLOYMENT: the worker's alone; its docker
# driver's machines run it) ($3: serve, $4: hostd, $5: 1 on, 0 off), and its
# secrets: every one
# the template names, which SSM must hold ($6, a JSON array of parameter
# names), and each optional one its `opennova.optional-secrets` label names
# (name to parameter ARN: the first admin, the previous edge secret, billing's
# set) while SSM holds it.
# shellcheck disable=SC2016  # jq's variables
REVISION_JQ='
def param: sub("^arn:[^:]*:ssm:[^:]*:[0-9]*:parameter"; "");
.taskDefinition as $t
| ($t | {family, taskRoleArn, executionRoleArn, networkMode, containerDefinitions, volumes,
         requiresCompatibilities, cpu, memory, runtimePlatform, ephemeralStorage, pidMode, ipcMode}
   | with_entries(select(.value != null)))
| .containerDefinitions |= map(
    . as $c
    | .image = ($images[$c.name] // error("the release has no image for container \($c.name)"))
    | if ((.environment // []) | any(.name == "ONNET_HOSTED_PROVIDER")) then
        ((.environment // []) | any(.name == "ONNET_HOSTED_DEPLOYMENT")) as $driver
        | .environment = ([.environment[] | select(.name != "ONNET_HOSTED_SERVE_IMAGE" and .name != "ONNET_HOSTED_HOSTD_IMAGE")
                          | if .name == "ONNET_HOSTED_PROVIDER" and $hosting == "0" then .value = "none" else . end]
                        + (if $serve == "" then [] else [{name: "ONNET_HOSTED_SERVE_IMAGE", value: $serve}] end)
                        + (if $hostd == "" or ($driver | not) then [] else [{name: "ONNET_HOSTED_HOSTD_IMAGE", value: $hostd}] end)
                        | sort_by(.name))
      else . end
    | .secrets = ([(.secrets // [])[] | . as $s | ($s.valueFrom | param) as $p
                   | if ($present | index($p)) != null then $s
                     else error("SSM holds no \($p) (the \($s.name) of \($c.name)): ./deploy/run.sh --deployment \($dep) secrets sync") end]
                  + [((.dockerLabels // {})["opennova.optional-secrets"] // "{}" | fromjson | to_entries[])
                     | (.value | param) as $p | select(($present | index($p)) != null) | {name: .key, valueFrom: .value}]
                  | sort_by(.name)))
| . + (if ($tags | length) > 0 then {tags: $tags} else {} end)'

# What a registered revision runs, for comparing two: the same fields, sorted.
REVISION_SHAPE_JQ='
.taskDefinition
| {family, taskRoleArn, executionRoleArn, networkMode, containerDefinitions, volumes,
   requiresCompatibilities, cpu, memory, runtimePlatform, ephemeralStorage, pidMode, ipcMode}
| with_entries(select(.value != null))
| .containerDefinitions |= map((.environment // []) |= sort_by(.name) | (.secrets // []) |= sort_by(.name))'

# A revision of $1 (a program or backup) for the release: the current one ($2)
# when it already runs exactly that, else a new one registered from
# Terraform's template. Prints its ARN.
release_revision() {
  local key="$1" current="$2" images="$3" serve="$4" hostd="$5" hosting="$6" present="$7"
  local template described input shape_new shape_cur
  template="$(task_template "$key")"
  described="$(aws ecs describe-task-definition --task-definition "$template" --include TAGS --output json)" \
    || die "describing $template failed"
  input="$(jq -c --argjson images "$images" --arg serve "$serve" --arg hostd "$hostd" --arg hosting "$hosting" --arg dep "$DEP" \
             --argjson present "$present" --argjson tags "$(jq -c '.tags // []' <<<"$described")" \
             "$REVISION_JQ" <<<"$described")" \
    || die "the $key revision cannot be made from $template (above)"
  if [ -n "$current" ] && [ "$current" != None ]; then
    shape_new="$(jq -cS '{taskDefinition: .} | '"$REVISION_SHAPE_JQ" <<<"$input")"
    shape_cur="$(aws ecs describe-task-definition --task-definition "$current" --output json | jq -cS "$REVISION_SHAPE_JQ")" || shape_cur=""
    if [ "$shape_new" = "$shape_cur" ]; then
      log "  $key: $current runs it already"
      printf '%s' "$current"
      return
    fi
  fi
  local file="$SECRETS_DIR/revision.$key.json" arn
  printf '%s' "$input" > "$file"
  arn="$(aws ecs register-task-definition --cli-input-json "file://$file" \
           --query taskDefinition.taskDefinitionArn --output text)" \
    || { rm -f "$file"; die "registering the $key revision failed"; }
  rm -f "$file"
  log "  $key: registered $arn"
  printf '%s' "$arn"
}

# How long a deployment may take before the toolbox stops waiting for it: past
# the circuit breaker's floor (10 failed task launches, each a pull, a boot and
# the health check's start period) before it rolls a bad revision back.
ROLL_TIMEOUT="${OPENNOVA_ROLL_TIMEOUT:-2400}"

# Each program's task counts (infra.env: min, desired, max), for the first
# release's start.
program_counts() {
  jq -c --arg p "$1" '.[$p] // empty' <<<"$(infra_get OPENNOVA_PROGRAM_COUNTS)" \
    || die "infra.env's OPENNOVA_PROGRAM_COUNTS is no JSON object (infra apply)"
}

# Point service $1 at task definition $2 (unless it runs it already: $3 force
# a new deployment anyway) and follow the deployment that makes until ECS calls
# it COMPLETED or FAILED (the circuit breaker's rollback), up to ROLL_TIMEOUT.
# A service at 0 tasks with no `app down` recorded is one before the first
# release, and so is one whose program the down record names no count for (a
# program added since `app down`, at the cutover the fleet and data): it starts
# at its desired count (OPENNOVA_PROGRAM_COUNTS), and its floor is raised to
# its min (Terraform's next apply sets the same). A daemon (hostd) has no count
# of its own (ECS runs one task per machine) and none of this: it is pointed at
# $2 alone, in the fleet cluster, and a deployment of it that wants no task (no
# machine has registered yet) is done.
service_roll() {
  local program="$1" td="$2" force="${3:-0}" cluster described current desired out dep_id waited=0 state counts record
  cluster="$(program_cluster "$program")"
  described="$(aws ecs describe-services --cluster "$cluster" --services "$program" --output json)" \
    || { err "describing the $program service failed"; return 1; }
  current="$(jq -r '.services[0].taskDefinition // ""' <<<"$described")"
  desired="$(jq -r '.services[0].desiredCount // 0' <<<"$described")"
  local args=(--cluster "$cluster" --service "$program" --task-definition "$td")
  local down_rc=0 first=0 daemon=0
  if is_daemon "$program"; then daemon=1; fi
  if [ "$daemon" -eq 0 ] && [ "$desired" -eq 0 ]; then
    record="$(down_record)" || down_rc=$?
    [ "$down_rc" -ne 1 ] || return 1
    if [ "$down_rc" -eq "$DOWN_NONE" ] || ! jq -e --arg p "$program" '.counts | has($p)' <<<"$record" >/dev/null 2>&1; then
      first=1
    fi
  fi
  if [ "$first" -eq 1 ]; then
    counts="$(program_counts "$program")"
    [ -n "$counts" ] || { err "infra.env names no task counts for $program (infra apply)"; return 1; }
    args+=(--desired-count "$(jq -r '.desired' <<<"$counts")")
    log "$program: its first release, at $(jq -r '.desired' <<<"$counts") task(s)"
  elif [ "$current" = "$td" ] && [ "$force" -eq 0 ]; then
    log "$program: runs ${td##*/} already"
    return 0
  fi
  [ "$force" -eq 0 ] || args+=(--force-new-deployment)
  out="$(aws ecs update-service "${args[@]}" --output json)" \
    || { err "updating the $program service failed"; return 1; }
  dep_id="$(jq -r '[.service.deployments[] | select(.status == "PRIMARY")][0].id // ""' <<<"$out")"
  [ -n "$dep_id" ] || { err "the $program service names no new deployment"; return 1; }
  log "$program: rolling to ${td##*/} (deployment $dep_id; up to ${ROLL_TIMEOUT}s)…"
  while :; do
    out="$(aws ecs describe-services --cluster "$cluster" --services "$program" --output json 2>/dev/null || echo '{}')"
    state="$(jq -r --arg d "$dep_id" '[.services[0].deployments[]? | select(.id == $d)][0].rolloutState // "GONE"' <<<"$out")"
    case "$state" in
      COMPLETED) break ;;
      FAILED)
        err "$program's deployment of ${td##*/} failed: $(jq -r --arg d "$dep_id" '[.services[0].deployments[]? | select(.id == $d)][0].rolloutStateReason // "?"' <<<"$out") (app logs $program)"
        return 1 ;;
      GONE)
        err "$program's deployment $dep_id is gone: another replaced it, or the circuit breaker rolled it back (app status)"
        return 1 ;;
    esac
    if [ "$daemon" -eq 1 ] && [ "$waited" -gt 0 ] && jq -e --arg d "$dep_id" \
         '[.services[0].deployments[]? | select(.id == $d)][0] | .desiredCount == 0 and .runningCount == 0 and .pendingCount == 0' \
         <<<"$out" >/dev/null 2>&1; then
      log "$program: no machine runs a task of it yet (the fleet cluster has no container instance)"
      break
    fi
    if [ "$waited" -ge "$ROLL_TIMEOUT" ]; then
      err "$program's deployment of ${td##*/} is still $state after ${waited}s (app status, app logs $program)"
      return 1
    fi
    if [ $((waited % 60)) -eq 0 ] && [ "$waited" -gt 0 ]; then
      log "  $program: $state, $(jq -r --arg d "$dep_id" '[.services[0].deployments[]? | select(.id == $d)][0] | "\(.runningCount) of \(.desiredCount) running, \(.failedTasks) failed"' <<<"$out")"
    fi
    sleep 15
    waited=$((waited + 15))
  done
  if [ -n "${counts:-}" ]; then
    aws application-autoscaling register-scalable-target --service-namespace ecs \
      --resource-id "service/$cluster/$program" --scalable-dimension ecs:service:DesiredCount \
      --min-capacity "$(jq -r '.min' <<<"$counts")" --max-capacity "$(jq -r '.max' <<<"$counts")" >/dev/null \
      || err "raising $program's floor to its min failed (the next infra apply sets it)"
  fi
  log "$program: ${td##*/} serves"
}

# How long the machines' agents get, once hostd's deployment completed, to report
# the release's hostd image: every ready machine's syncs at most every 5 s, and
# a new agent's first one comes once its task starts.
HOSTD_CONFIRM_TIMEOUT=180

# Whether every ready machine's agent reports hostd image $1 (the release's
# reference, as its ECS task definition names it: the admin fleet view's
# agent_image, which hostd reads from its task's metadata), asked every 15 s
# up to HOSTD_CONFIRM_TIMEOUT. With no hostd task running (no machine) there
# is no machine to confirm; when the first ask fails (the cutover: no web, or
# no fleet program yet; or ECS Exec into web refused) nothing is confirmed, and
# it says which failed. Either way the deploy goes on. A later ask that fails
# is asked again; at the timeout the last ask's own outcome is the reason: its
# failure, or the machines it found on another image.
hostd_confirm() {
  local image="$1" running json stale waited=0 errors failed=""
  [ -n "$image" ] || { err "hostd: the release names no hostd image to confirm"; return 1; }
  running="$(aws ecs describe-services --cluster "$(fleet_cluster)" --services hostd \
               --query 'services[0].runningCount' --output text 2>/dev/null || echo 0)"
  if ! [[ "$running" =~ ^[0-9]+$ ]] || [ "$running" -eq 0 ]; then
    log "hostd: no machine to confirm (no hostd task runs)"
    return 0
  fi
  errors="$(mktemp)"
  while :; do
    if json="$(admin_api_json GET /api/admin/hosted 2>"$errors")"; then
      failed=""
      stale="$(jq -r --arg i "$image" '[.machines[]? | select(.state == "ready" and .agent_image != $i) | .id | tostring] | join(" ")' <<<"$json" 2>/dev/null)" \
        || stale="?"
      if [ -z "$stale" ]; then
        log "hostd: every ready machine's agent reports ${image##*@}"
        rm -f "$errors"
        return 0
      fi
    elif [ "$waited" -eq 0 ]; then
      log "hostd: the machines' agents are not confirmed on ${image##*@}, the service could not be asked: $(admin_api_failure "$errors")"
      rm -f "$errors"
      return 0
    else
      failed="$(admin_api_failure "$errors")"
    fi
    if [ "$waited" -ge "$HOSTD_CONFIRM_TIMEOUT" ]; then
      if [ -n "$failed" ]; then
        err "hostd: after ${waited}s the machines' agents are not confirmed on ${image##*@}: the last ask failed: $failed (app logs web, fleet status)"
      else
        err "hostd: after ${waited}s, machine(s) ${stale:-?} still report another agent image than ${image##*@} (fleet status, app logs hostd)"
      fi
      rm -f "$errors"
      return 1
    fi
    sleep 15
    waited=$((waited + 15))
  done
}

# Roll every program to the release's revisions ($1, the manifest), in the
# deploy's order. The first that fails stops it, leaving the rest on what they
# ran. hostd, once its deployment completed, is confirmed on the machines
# (hostd_confirm, with the manifest's hostd image); a deployment or a
# confirmation that fails points it back at the revision it ran before, so
# the programs after it (the fleet program first) never meet agents of a release
# they do not run. A release that names no hostd revision (none was made: no
# hostd image, hosting off) leaves hostd on what it runs.
release_roll() {
  local manifest="$1" program td before
  run_programs
  for program in "${RUN_PROGRAMS[@]}"; do
    td="$(jq -r --arg p "$program" '.task_definitions[$p] // empty' <<<"$manifest")"
    if [ -z "$td" ] && is_daemon "$program"; then
      log "$program: the release names no revision of it; it keeps what it runs"
      continue
    fi
    [ -n "$td" ] || { err "the release names no task definition for $program"; return 1; }
    if ! is_daemon "$program"; then
      service_roll "$program" "$td" || return 1
      continue
    fi
    before="$(service_task_definition "$program")" || return 1
    if service_roll "$program" "$td" \
         && { [ "$before" = "$td" ] || hostd_confirm "$(jq -r '.images.hostd // empty' <<<"$manifest")"; }; then
      continue
    fi
    if [ -n "$before" ] && [ "$before" != None ] && [ "$before" != "$td" ]; then
      err "$program: pointing it back at ${before##*/}, the revision it ran before"
      service_roll "$program" "$before" || err "$program did not come back on ${before##*/} (app status)"
    fi
    return 1
  done
}

# Make manifest $2 (id $1) the deployment's release, once every program runs
# it: S3 keeps every one, SSM the current.
release_record() {
  local id="$1" manifest="$2" bucket
  bucket="$(infra_get OPENNOVA_OPS_BUCKET)"
  [ "${#manifest}" -lt 4096 ] || die "release $id's manifest is ${#manifest} bytes, past the 4 KiB an SSM parameter holds"
  printf '%s' "$manifest" | aws s3 cp - "s3://$bucket/releases/$id.json" --only-show-errors \
    --content-type application/json \
    || die "every program runs release $id, but uploading its manifest failed: run app deploy again (it registers nothing new) to record it"
  aws ssm put-parameter --name "$SSM_PREFIX/release" --type String --value "$manifest" --overwrite >/dev/null \
    || die "every program runs release $id, but pointing $SSM_PREFIX/release at it failed: run app deploy again to record it"
  backup_schedule_point "$(jq -r '.task_definitions.backup // empty' <<<"$manifest")" \
    || die "release $id is recorded, but the nightly backup still runs the previous release's revision: run app deploy again (it registers nothing new) to point it"
}

# Point the nightly backup's schedule (infra.env OPENNOVA_BACKUP_SCHEDULE,
# infra/aws/backup.tf) at the release's backup revision $1, its image pinned
# by digest, and switch it on: the schedule never runs the family's newest
# revision, which an infra apply's template (`latest`) may be. Terraform's
# plan reads the same revision from the release, so an apply leaves it be.
backup_schedule_point() {
  local td="$1" name described input
  [[ "$td" == arn:*:task-definition/*:* ]] || { err "the release names no backup revision"; return 1; }
  name="$(infra_get OPENNOVA_BACKUP_SCHEDULE)"
  [ -n "$name" ] || { err "infra.env names no backup schedule (infra apply)"; return 1; }
  described="$(aws scheduler get-schedule --name "$name" --output json)" \
    || { err "reading the $name schedule failed"; return 1; }
  if jq -e --arg td "$td" '.State == "ENABLED" and .Target.EcsParameters.TaskDefinitionArn == $td' <<<"$described" >/dev/null; then
    log "nightly backup: runs ${td##*/} already"
    return 0
  fi
  # update-schedule replaces the whole schedule: everything get-schedule
  # gave back but what it alone reports (its ARN and dates), the revision
  # swapped in.
  input="$(jq -c --arg td "$td" '{Name, GroupName, Description, ScheduleExpression, ScheduleExpressionTimezone,
             StartDate, EndDate, KmsKeyArn, ActionAfterCompletion, FlexibleTimeWindow, Target}
           | with_entries(select(.value != null)) | .State = "ENABLED"
           | .Target.EcsParameters.TaskDefinitionArn = $td' <<<"$described")" \
    || { err "the $name schedule does not read"; return 1; }
  aws scheduler update-schedule --cli-input-json "$input" >/dev/null \
    || { err "updating the $name schedule failed"; return 1; }
  log "nightly backup: runs ${td##*/}"
}

# The current release's images and its id, as JSON (`app images`, what
# `app deploy --images-from` and --promote take): {deployment, release, images}.
release_images_json() {
  local manifest
  manifest="$(release_current_manifest)"
  jq -e '(.id | type) == "string" and (.images | type) == "object"' <<<"$manifest" >/dev/null \
    || die "$DEP has no release of the programs yet (app deploy)"
  jq -c --arg d "$DEP" '{deployment: $d, release: .id, images: .images}' <<<"$manifest"
}

# Images pinned by another release ($1: release_images_json's form) as
# image_tags digests: every image of the release, each in this deployment's
# registry and pinned by digest, or the deploy stops before it changes anything.
declare -A image_tags=()
pin_release_images() {
  local pinned="$1" registry key ref
  jq -e '(.deployment | type) == "string" and (.release | type) == "string" and (.images | type) == "object"' <<<"$pinned" >/dev/null 2>&1 \
    || die "the pinned images are no {deployment, release, images} object (app images prints one)"
  registry="$(infra_get OPENNOVA_IMAGE_REGISTRY)"
  for key in "${SERVICE_IMAGES[@]}" serve hostd; do
    ref="$(jq -r --arg k "$key" '.images[$k] // empty' <<<"$pinned")"
    if [ -z "$ref" ]; then
      case "$key" in
        serve|hostd) log "  $key: the pinned release names none; the current release's is kept"; continue ;;
        *) die "the pinned release names no $key image; nothing was changed" ;;
      esac
    fi
    if [ "${ref%@*}" != "$registry/$(image_repo "$key")" ] || ! [[ "${ref##*@}" =~ ^sha256:[0-9a-f]{64}$ ]]; then
      die "the pinned $key image '$ref' is not $registry/$(image_repo "$key")@sha256:<digest>; nothing was changed"
    fi
    image_tags["$key"]="@${ref##*@}"
  done
}

app_deploy() {
  local tag="" serve_tag="" hostd_tag="" hosting=1 keep_hosting=0 key value promote="" images_from="" yes=0
  image_tags=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --tag|--serve-tag|--hostd-tag|--image)
        if [ $# -lt 2 ] || [ -z "$2" ]; then die "$1 needs a value"; fi
        case "$1" in
          --tag) tag="$2" ;;
          --serve-tag) serve_tag="$2" ;;
          --hostd-tag) hostd_tag="$2" ;;
          --image)
            [[ "$2" == *=* ]] || die "--image takes NAME=TAG (gate=sha-abc1234)"
            key="${2%%=*}"; value="${2#*=}"
            image_repo "$key" >/dev/null
            image_tags["$key"]="$value" ;;
        esac
        shift 2 ;;
      --promote|--images-from)
        if [ $# -lt 2 ] || [ -z "$2" ]; then die "$1 needs a value"; fi
        if [ "$1" = --promote ]; then promote="$2"; else images_from="$2"; fi
        shift 2 ;;
      --no-hosting) hosting=0; shift ;;
      --keep-hosting) keep_hosting=1; shift ;;
      --yes) yes=1; shift ;;
      *) die "unknown: app deploy $1 (try: --tag T, --image NAME=T, --serve-tag T, --hostd-tag T, --promote staging, --images-from FILE|-, --no-hosting, --yes)" ;;
    esac
  done
  [ -z "$serve_tag" ] || image_tags[serve]="$serve_tag"
  [ -z "$hostd_tag" ] || image_tags[hostd]="$hostd_tag"

  aws_auth
  infra_env_load
  # A promotion: exactly the images another release runs, by digest (staging's
  # rehearsed release into prod: the images are never built again).
  local pinned="" promoted_from=null
  if [ -n "$promote" ] && [ -n "$images_from" ]; then die "--promote or --images-from, not both"; fi
  if [ -n "$promote" ]; then
    if [ "$DEP" != prod ] || [ "$promote" != staging ]; then
      die "--promote takes staging, into prod alone: staging's images reach prod this way, and prod's never go back"
    fi
    log "reading staging's current release (its account)…"
    pinned="$(as_deployment staging release_images_json)" || die "reading staging's release failed; nothing was changed"
  elif [ "$images_from" = - ]; then
    pinned="$(cat)"
  elif [ -n "$images_from" ]; then
    [ -f "$images_from" ] || die "no file $images_from"
    pinned="$(cat "$images_from")"
  fi
  if [ -n "$pinned" ]; then
    if [ -n "$tag" ] || [ "${#image_tags[@]}" -gt 0 ]; then
      die "--promote and --images-from pin every image: no --tag, --image, --serve-tag or --hostd-tag beside them"
    fi
    pin_release_images "$pinned"
    promoted_from="$(jq -c '{deployment, release}' <<<"$pinned")"
    log "promoting $(jq -r '.deployment' <<<"$pinned")'s release $(jq -r '.release' <<<"$pinned"): its images, by digest"
  fi
  local manifest current_id hosted_provider
  manifest="$(release_current_manifest)"
  current_id="$(jq -r '.id // "none"' <<<"$manifest")"
  if [ "$keep_hosting" -eq 1 ] && [ "$(jq -r '.hosting // true' <<<"$manifest")" = false ]; then hosting=0; fi
  hosted_provider="${INFRA[ONNET_HOSTED_PROVIDER]:-none}"
  log "deployment $DEP, current release: $current_id"

  local images="{}" ref
  for key in "${SERVICE_IMAGES[@]}"; do
    ref="$(pick_image "$key" "${image_tags[$key]:-$tag}" "$manifest")" || die "nothing was changed"
    images="$(jq -c --arg k "$key" --arg v "$ref" '. + {($k): $v}' <<<"$images")"
    log "  $key $ref"
  done
  local serve hostd
  if ! serve="$(pick_image serve "${image_tags[serve]:-}" "$manifest")"; then
    if [ "$hosted_provider" != none ] && [ "$hosting" -eq 1 ]; then
      die "the hosted servers need the serve image; nothing was changed (or deploy with --no-hosting)"
    fi
    serve=""
  fi
  if ! hostd="$(pick_image hostd "${image_tags[hostd]:-}" "$manifest")"; then
    if [ "$hosted_provider" != none ] && [ "$hosting" -eq 1 ]; then
      die "the machines need hostd's image; nothing was changed (or deploy with --no-hosting)"
    fi
    hostd=""
  fi
  log "  serve  ${serve:-none}"
  log "  hostd  ${hostd:-none}"
  if [ "$hosting" -eq 0 ]; then log "  hosted game servers: off (--no-hosting)"; else log "  hosted game servers: $hosted_provider (infra.env)"; fi
  local what="these images"
  [ "$promoted_from" = null ] || what="$(jq -r '.deployment + "'"'"'s release " + .release' <<<"$promoted_from")"
  confirm_prod "roll prod's programs onto $what" "$yes"

  # Each program's revision, the SSM secrets it names (every one SSM must hold,
  # the optional ones while it does); hostd's container takes the hostd image.
  local present revisions="{}" td revision_images
  present="$(ssm_secret_names | jq -R . | jq -sc .)"
  revision_images="$(jq -c --arg h "$hostd" 'if $h == "" then . else . + {hostd: $h} end' <<<"$images")"
  run_programs
  log "revisions:"
  for key in "${RUN_PROGRAMS[@]}"; do
    if is_daemon "$key" && [ -z "$hostd" ]; then
      log "  $key: no hostd image (--no-hosting); it keeps what it runs"
      continue
    fi
    td="$(release_revision "$key" "$(service_task_definition "$key")" "$revision_images" "$serve" "$hostd" "$hosting" "$present")" || exit 1
    revisions="$(jq -c --arg k "$key" --arg v "$td" '. + {($k): $v}' <<<"$revisions")"
  done

  # The commit and infra/aws's state as deploy/run.sh read them on the host
  # (a linked worktree's .git names a directory outside the mount), else as
  # this checkout's git reads them.
  local rev="${OPENNOVA_REVISION:-}" dirty="${OPENNOVA_INFRA_DIRTY:-false}" id
  if [ -z "$rev" ]; then
    rev="$(git -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
    dirty=false
    if [ -n "$(git -C "$ROOT" status --porcelain -- infra/aws 2>/dev/null)" ]; then dirty=true; fi
  fi
  [ "$dirty" = true ] || dirty=false
  id="$(date -u +%Y%m%dT%H%M%SZ)-${rev:0:7}"
  # Rolled first, recorded once every program runs it: the release parameter
  # (and the backup task's revision, which the nightly run takes) never names
  # what nothing runs.
  if ! release_roll "$(jq -nc --argjson r "$revisions" --arg hostd "$hostd" '{task_definitions: $r, images: {hostd: $hostd}}')"; then
    die "the deployment did not reach $id, which is not recorded: $current_id stays the deployment's release, and the programs before the one that failed run the new revisions. ./deploy/run.sh$DEP_FLAG app rollback --to $current_id puts every program back on $current_id"
  fi
  td="$(release_revision backup "$(jq -r '.task_definitions.backup // empty' <<<"$manifest")" "$images" "$serve" "$hostd" "$hosting" "$present")" || exit 1
  revisions="$(jq -c --arg v "$td" '. + {backup: $v}' <<<"$revisions")"
  local new
  new="$(jq -nc --arg id "$id" --arg dep "$DEP" --arg created "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
           --arg rev "$rev" --argjson dirty "$dirty" --arg previous "$current_id" \
           --argjson images "$images" --arg serve "$serve" --arg hostd "$hostd" \
           --argjson hosting "$([ "$hosting" -eq 1 ] && echo true || echo false)" \
           --arg programs "${RUN_PROGRAMS[*]}" --argjson revisions "$revisions" \
           --argjson promoted_from "$promoted_from" \
    '{id: $id, deployment: $dep, created: $created, revision: $rev, dirty: $dirty,
      previous: (if $previous == "none" then null else $previous end),
      promoted_from: $promoted_from,
      programs: ($programs | split(" ")), hosting: $hosting,
      images: ($images + (if $serve == "" then {} else {serve: $serve} end)
                       + (if $hostd == "" then {} else {hostd: $hostd} end)),
      task_definitions: $revisions}')"
  release_record "$id" "$new"
  log "deploy complete: $id is the deployment's release (was $current_id)"
}

app_releases() {
  aws_auth
  infra_env_load
  local bucket current keys key id m
  bucket="$(infra_get OPENNOVA_OPS_BUCKET)"
  current="$(jq -r '.id // "none"' <<<"$(release_current_manifest)")"
  keys="$(aws s3api list-objects-v2 --bucket "$bucket" --prefix "releases/" \
            --query 'Contents[].Key' --output text 2>/dev/null)" || die "listing s3://$bucket/releases/ failed"
  tr '\t' '\n' <<<"$keys" | grep '\.json$' | sort | tail -n 20 | while read -r key; do
    id="$(basename "$key" .json)"
    m="$(aws s3 cp "s3://$bucket/$key" - 2>/dev/null || echo '{}')"
    printf '%s %s  %s  rev %s%s%s%s\n' "$([ "$id" = "$current" ] && echo '*' || echo ' ')" "$id" \
      "$(jq -r '.created // "?"' <<<"$m")" "$(jq -r '.revision // "?"' <<<"$m")" \
      "$(jq -r 'if .dirty then " (dirty)" else "" end' <<<"$m")" \
      "$(jq -r 'if .promoted_from then " (promoted: \(.promoted_from.deployment)'"'"'s \(.promoted_from.release))" else "" end' <<<"$m")" \
      "$(jq -r 'if (.programs // null) == null then " (before the programs)" else "" end' <<<"$m")"
    jq -r '.images // {} | to_entries[] | "      \(.key) \(.value | sub("^ghcr.io/"; ""))"' <<<"$m"
  done
  log "current: $current (* above); the newest 20 shown"
}

# Point every program back at a recorded release's revisions.
app_rollback() {
  local to="" yes=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --to) [ $# -ge 2 ] || die "--to needs a release id"; to="$2"; shift 2 ;;
      --yes) yes=1; shift ;;
      *) die "unknown: app rollback $1 (try: --to ID, --yes)" ;;
    esac
  done
  aws_auth
  infra_env_load
  local bucket current manifest
  bucket="$(infra_get OPENNOVA_OPS_BUCKET)"
  current="$(release_current_manifest)"
  if [ -z "$to" ]; then
    to="$(jq -r '.previous // empty' <<<"$current")"
    [ -n "$to" ] || die "the current release names no previous release: app rollback --to ID (app releases lists them)"
  fi
  [[ "$to" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$ ]] || die "'$to' is no release id"
  manifest="$(release_manifest "$bucket" "$to")" || exit 1
  # A release of the single service instance runs on no ECS service: going
  # back past the cutover is the cutover's rollback. Nor does one from before
  # the program split (the controller's: no fleet, no data program): its
  # revisions name a program no service runs, and no fleet would lead.
  jq -e '(.programs | type) == "array" and (.task_definitions | type) == "object"' <<<"$manifest" >/dev/null 2>&1 \
    || die "release $to was made before the programs (its manifest names none): no program runs it"
  jq -e '(.programs | index("fleet")) != null and (.programs | index("data")) != null' <<<"$manifest" >/dev/null 2>&1 \
    || die "release $to was made before the program split (its programs name no fleet and no data program, the controller's era): no service runs its controller, so rolling back to it would leave the fleet unled. Roll forward instead (app deploy); going back past the split is the cutover's rollback (DEPLOY.md)"
  manifest="$(jq -c . <<<"$manifest")"
  confirm_prod "roll prod's programs back onto release $to" "$yes"
  # Rolled first, recorded once every program runs it.
  release_roll "$manifest" \
    || die "the deployment did not reach $to, which is not recorded: $(jq -r '.id // "none"' <<<"$current") stays the deployment's release (app status says what each program runs)"
  aws ssm put-parameter --name "$SSM_PREFIX/release" --type String --value "$manifest" --overwrite >/dev/null \
    || die "every program runs release $to, but pointing $SSM_PREFIX/release at it failed: run app rollback --to $to again to record it"
  backup_schedule_point "$(jq -r '.task_definitions.backup // empty' <<<"$manifest")" \
    || die "release $to is recorded, but the nightly backup still runs the rolled-back release's revision: run app rollback --to $to again to point it"
  log "rollback complete: $to is the deployment's release again (was $(jq -r '.id // "none"' <<<"$current"); the nightly backup runs its backup revision)"
}

# Every program's service, in PROGRAMS' order: the deployment's cluster's, and
# hostd from the fleet cluster (its desired count is the machines' number).
app_status() {
  aws_auth
  infra_env_load
  local cluster fleet manifest scaled
  cluster="$(ecs_cluster)"
  fleet="$(fleet_cluster)"
  manifest="$(release_current_manifest)"
  run_programs
  mapfile -t scaled < <(scaled_programs)
  log "deployment $DEP, cluster $cluster${fleet:+, fleet cluster $fleet}, release $(jq -r '.id // "none"' <<<"$manifest")"
  {
    aws ecs describe-services --cluster "$cluster" --services "${scaled[@]}" --output json
    [ -z "$fleet" ] || aws ecs describe-services --cluster "$fleet" --services "${DAEMON_PROGRAMS[@]}" --output json
  } | jq -rs --argjson m "$manifest" --arg order "${RUN_PROGRAMS[*]}" '
        def pad($n): tostring | if length < $n then . + (" " * ($n - length)) else . end;
        def row: [.[0] | pad(12)] + [.[1], .[2], .[3] | pad(9)] + [.[4] | pad(13)] + [.[5] | pad(40)] + [.[6] | tostring] | join("");
        (["PROGRAM", "DESIRED", "RUNNING", "PENDING", "ROLLOUT", "TASK DEFINITION", "THE RELEASE'"'"'S"] | row),
        ([.[].services[]] | sort_by(.serviceName as $n | $order | split(" ") | index($n)) | .[] | . as $s
          | [.serviceName, .desiredCount, .runningCount, .pendingCount,
             ([.deployments[] | select(.status == "PRIMARY")][0].rolloutState // "?"),
             (.taskDefinition | split("/") | last),
             (if ($m.task_definitions[$s.serviceName] // "") == .taskDefinition then "yes" else "no" end)] | row)'
}

app_logs() {
  local program="" since=1h follow=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --follow|-f) follow=(--follow); shift ;;
      --since) [ $# -ge 2 ] || die "--since needs a value (10m, 2h, 1d)"; since="$2"; shift 2 ;;
      -*) die "unknown: app logs $1 (try: [PROGRAM|backup] [--follow] [--since 1h])" ;;
      *) program="$1"; shift ;;
    esac
  done
  aws_auth
  infra_env_load
  local streams=() group
  group="$(infra_get OPENNOVA_LOG_GROUP)"
  if [ -n "$program" ]; then
    [ "$program" = backup ] || program="$(program_name "$program")" || exit 1
    streams=(--log-stream-name-prefix "$program/")
    # hostd logs to the machines' group, a stream per task: hostd/hostd/<task id>.
    if is_daemon "$program"; then group="$(infra_get OPENNOVA_MACHINES_LOG_GROUP)"; fi
  fi
  aws logs tail "$group" ${streams[@]+"${streams[@]}"} --since "$since" --format short ${follow[@]+"${follow[@]}"}
}

# app shell PROGRAM [CONTAINER]: a root shell in a running task's container
# (ECS Exec; web's second container is `portal`).
app_shell() {
  if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    die "app shell takes a PROGRAM (gate, lobby, web, fleet, worker, data) and, for web, a CONTAINER (web, portal)"
  fi
  local program container
  program="$(program_name "$1")" || exit 1
  ! is_daemon "$program" || die "$program runs a task on every machine: ./deploy/run.sh$DEP_FLAG fleet shell MACHINE opens a shell in one machine's"
  container="${2:-$program}"
  case "$program/$container" in
    web/portal|"$program/$program") ;;
    *) die "$program's task has no container $container" ;;
  esac
  aws_auth
  aws ecs execute-command --cluster "$(ecs_cluster)" --task "$(program_task "$program")" \
    --container "$container" --interactive --command /bin/sh
}

# app down / up: every service's count to 0, the counts kept in SSM
# (.../down) and Application Auto Scaling held off meanwhile; up puts them
# back, a program the record names no count for (one added since the down) at
# its desired count (OPENNOVA_PROGRAM_COUNTS). down keeps an earlier record (a
# second down records no zeros). The reverse of the deploy's order. A daemon
# (hostd) has no count: down and up leave it running on the machines, their
# servers with it, and it syncs again once the fleet program is back.
DOWN_ORDER=(gate lobby web fleet hostd data worker)
scaling_suspended() {
  local program="$1" on="$2"
  aws application-autoscaling register-scalable-target --service-namespace ecs \
    --resource-id "service/$(ecs_cluster)/$program" --scalable-dimension ecs:service:DesiredCount \
    --suspended-state "DynamicScalingInSuspended=$on,DynamicScalingOutSuspended=$on,ScheduledScalingSuspended=$on" >/dev/null \
    || die "holding $program's scaling (suspended=$on) failed"
}

# The counts `app down` recorded (SSM .../down), on stdout: 0 when there is a
# record, 3 when there is none. Any other failure to read it (a refusal, no
# answer) is 1, after its reason on stderr: zero counts with no record are a
# first release's, and a misread marker would start a deployment someone took
# down, so a caller stops on 1.
DOWN_NONE=3
down_record() {
  local out rc=0
  out="$(aws ssm get-parameter --name "$SSM_PREFIX/down" --query Parameter.Value --output text 2>&1)" || rc=$?
  if [ "$rc" -eq 0 ]; then
    printf '%s' "$out"
    return 0
  fi
  if [[ "$out" == *ParameterNotFound* ]]; then
    return "$DOWN_NONE"
  fi
  err "reading $SSM_PREFIX/down failed, so whether the programs are down is unknown: $(head -n 1 <<<"$out")"
  return 1
}

programs_down() {
  local cluster record counts program scaled rc=0
  cluster="$(ecs_cluster)"
  run_programs
  mapfile -t scaled < <(scaled_programs)
  record="$(down_record)" || rc=$?
  [ "$rc" -ne 1 ] || die "the programs' down marker does not read; nothing was changed"
  if [ "$rc" -eq 0 ]; then
    log "the programs are down already (since $(jq -r '.since // "?"' <<<"$record")); their counts stay recorded"
  else
    counts="$(aws ecs describe-services --cluster "$cluster" --services "${scaled[@]}" --output json \
                | jq -c '[.services[] | {(.serviceName): .desiredCount}] | add')" \
      || die "reading the services' counts failed"
    aws ssm put-parameter --name "$SSM_PREFIX/down" --type String --overwrite \
      --value "$(jq -nc --argjson c "$counts" --arg since "$(date -u +%Y-%m-%dT%H:%M:%SZ)" '{since: $since, counts: $c}')" >/dev/null \
      || die "recording the counts in $SSM_PREFIX/down failed; nothing was changed"
  fi
  for program in "${DOWN_ORDER[@]}"; do
    if is_daemon "$program"; then
      log "  $program: a daemon on the machines, left running"
      continue
    fi
    scaling_suspended "$program" true
    aws ecs update-service --cluster "$cluster" --service "$program" --desired-count 0 --output text --query 'service.serviceName' >/dev/null \
      || die "stopping $program failed"
    log "  $program: 0 tasks"
  done
  aws ecs wait services-stable --cluster "$cluster" --services "${scaled[@]}" \
    || err "not every service settled at 0 within the waiter's 10 minutes (app status)"
}

programs_up() {
  local cluster record count program scaled
  cluster="$(ecs_cluster)"
  run_programs
  mapfile -t scaled < <(scaled_programs)
  local rc=0
  record="$(down_record)" || rc=$?
  [ "$rc" -ne 1 ] || die "the programs' down marker does not read; nothing was changed"
  [ "$rc" -eq 0 ] \
    || die "no counts recorded ($SSM_PREFIX/down): the programs are not down (app status); infra apply sets a missing service's count"
  for program in "${scaled[@]}"; do
    count="$(jq -r --arg p "$program" '.counts[$p] // empty' <<<"$record")"
    if [ -z "$count" ]; then
      count="$(jq -r '.desired // empty' <<<"$(program_counts "$program")")"
      [ -n "$count" ] || die "the down record names no count for $program, nor does infra.env (infra apply); nothing more was started"
    fi
    aws ecs update-service --cluster "$cluster" --service "$program" --desired-count "$count" --output text --query 'service.serviceName' >/dev/null \
      || die "starting $program failed"
    scaling_suspended "$program" false
    log "  $program: $count task(s)"
  done
  aws ssm delete-parameter --name "$SSM_PREFIX/down" >/dev/null || err "deleting $SSM_PREFIX/down failed: delete it by hand"
  aws ecs wait services-stable --cluster "$cluster" --services "${scaled[@]}" \
    || { err "not every service came back within the waiter's 10 minutes (app status)"; return 1; }
}

cmd_app() {
  local action="${1:-status}"
  shift || true
  case "$action" in
    deploy)   app_deploy "$@" ;;
    releases) app_releases "$@" ;;
    rollback) app_rollback "$@" ;;
    converge)
      take_yes "$@"
      [ "${#OPT_REST[@]}" -eq 0 ] || die "unknown: app converge ${OPT_REST[0]} (try: --yes)"
      log "converge: the current release's images on Terraform's task definitions and the secrets SSM holds now"
      if [ "$OPT_YES" -eq 1 ]; then app_deploy --keep-hosting --yes; else app_deploy --keep-hosting; fi ;;
    images)
      [ $# -eq 0 ] || die "unknown: app images $1"
      aws_auth
      release_images_json
      printf '\n' ;;
    status)
      [ $# -eq 0 ] || die "unknown: app status $1"
      app_status ;;
    restart)
      take_yes "$@"
      local program="" p targets=()
      [ "${#OPT_REST[@]}" -le 1 ] || die "app restart takes one PROGRAM, or none for every program"
      [ "${#OPT_REST[@]}" -eq 0 ] || program="$(program_name "${OPT_REST[0]}")" || exit 1
      confirm_prod "restart ${program:-every program}'s tasks (new tasks, the same revision)" "$OPT_YES"
      aws_auth
      run_programs
      targets=("${RUN_PROGRAMS[@]}")
      if [ -n "$program" ]; then
        [[ " ${RUN_PROGRAMS[*]} " == *" $program "* ]] || die "$program does not run in $DEP (hosting is off: no fleet cluster)"
        targets=("$program")
      fi
      for p in "${targets[@]}"; do
        service_roll "$p" "$(service_task_definition "$p")" 1 || die "$p did not come back (app status)"
      done ;;
    down)
      take_yes "$@"
      [ "${#OPT_REST[@]}" -eq 0 ] || die "unknown: app down ${OPT_REST[0]} (try: --yes)"
      confirm_prod "stop every program's tasks (the site and the game service stay down until app up)" "$OPT_YES"
      aws_auth
      programs_down
      log "every program is down: ./deploy/run.sh$DEP_FLAG app up starts them again" ;;
    up)
      [ $# -eq 0 ] || die "unknown: app up $1"
      aws_auth
      programs_up || die "app up did not finish (app status)"
      log "every program is up" ;;
    logs)  app_logs "$@" ;;
    shell) app_shell "$@" ;;
    *) die "unknown: app $action (try: deploy|images|releases|rollback|converge|status|restart|down|up|logs|shell)" ;;
  esac
}
