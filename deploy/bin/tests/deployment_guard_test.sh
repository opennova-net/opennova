#!/usr/bin/env bash
# The toolbox's deployment guard (deploy/bin/on-deploy; DEPLOY.md,
# "Deployments: dev, staging and prod"), offline: a command that changes a
# deployment acts only on the one --deployment names, a read-only one reads
# prod unless told otherwise, the zone's, the organization's and the CI
# runners' commands take none, prod's changes ask for the typed word, data and
# images cross between the deployments in their one direction alone, a deploy
# never takes `app down`'s zero counts for a first release (but starts a
# program the down record names no count for, and `app up` the same), a
# rollback refuses a release from before the program split, a release's
# revisions hand the serve image to every container that reads the fleet's
# policy and hostd's to the machine driver's alone, and the org
# stack's first run moves its state before the vault learns of the bucket. It
# sources on-deploy (which then runs nothing) and stubs every command group,
# the vault and AWS, so nothing is read or changed anywhere. It needs bash and
# jq;
# .github/workflows/infra.yml runs it:
#   bash deploy/bin/tests/deployment_guard_test.sh
# pass and fail never fail, so `cond && pass || fail` is the if-then-else it reads as.
# shellcheck disable=SC2015
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
export OPENNOVA_ROOT="$root" OPENNOVA_SECRETS_DIR="$scratch/secrets"
unset OP_SERVICE_ACCOUNT_TOKEN

# shellcheck source=deploy/bin/on-deploy
source "$root/deploy/bin/on-deploy"
# on-deploy's own -e and EXIT trap are its run's, not this check's.
set +e
trap 'rm -rf "$scratch"' EXIT

# Every command group answers with the deployment it was handed.
for group in secrets stripe infra org zone github ci aws app db backup fleet; do
  eval "cmd_$group() { echo \"ran DEP=\${DEP:-none} DEFAULTED=\$DEP_DEFAULTED\"; }"
done

failures=0
pass() { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1"; failures=$((failures + 1)); }

# expect_run NAME WANT ARGS...: main ARGS runs its command with WANT (the
# command's output line).
expect_run() {
  local name="$1" want="$2" out
  shift 2
  out="$( (main "$@") 2>/dev/null)" || { fail "$name (refused: $( (main "$@") 2>&1 >/dev/null | tail -n 1))"; return; }
  if [ "$out" = "$want" ]; then pass "$name"; else fail "$name (got '$out', want '$want')"; fi
}

# expect_refused NAME TEXT ARGS...: main ARGS stops, naming TEXT, before any
# command runs.
expect_refused() {
  local name="$1" text="$2" out rc=0
  shift 2
  out="$( (main "$@") 2>&1)" || rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$name (ran: $out)"
  elif [[ "$out" == *"ran DEP="* ]]; then
    fail "$name (a command ran before the refusal)"
  elif [[ "$out" != *"$text"* ]]; then
    fail "$name (refused, but not for '$text': $out)"
  else
    pass "$name"
  fi
}

echo "--- a change names its deployment"
for cmd in "app deploy --tag sha-abc1234" "app rollback" "app converge" "app restart web" "app down" "app up" \
           "app shell web" "infra plan" "infra apply" "infra destroy" "infra import a b" "infra state-rm a" \
           "secrets init" "secrets sync" "stripe setup" "db bootstrap" "db psql" "db reset-db --yes-drop-database" \
           "db snapshot" "backup now" "backup restore novaworld/x.dump --yes-overwrite-database" "backup import -" \
           "fleet drain i-0" "fleet end i-0" "fleet roll" "fleet roll --dry-run" "fleet shell 12" \
           "aws s3 ls"; do
  # shellcheck disable=SC2086  # the words are the command's arguments
  expect_refused "'$cmd' without --deployment" "There is no default" $cmd
done
unset OPENNOVA_DEPLOYMENT
OPENNOVA_DEPLOYMENT=prod expect_refused "OPENNOVA_DEPLOYMENT names nothing" "There is no default" app deploy --tag x

echo "--- the named deployment, and no other"
expect_run "--deployment staging app deploy" "ran DEP=staging DEFAULTED=0" --deployment staging app deploy --tag x
expect_run "--deployment prod infra apply" "ran DEP=prod DEFAULTED=0" --deployment prod infra apply
expect_run "--deployment=staging aws" "ran DEP=staging DEFAULTED=0" --deployment=staging aws s3 ls
expect_refused "an unknown deployment" "prod or staging" --deployment dev app status
expect_refused "the old default workspace's name" "prod or staging" --deployment default app status

echo "--- a read-only command reads prod unless told, and says so"
for cmd in "app status" "app releases" "app logs web" "app images" "secrets check" "stripe check" \
           "infra output" "infra list" "fleet status" "fleet logs 12 3" "backup list"; do
  # shellcheck disable=SC2086
  expect_run "'$cmd' alone" "ran DEP=prod DEFAULTED=1" $cmd
done
expect_run "--deployment staging app status" "ran DEP=staging DEFAULTED=0" --deployment staging app status
banner_out="$( (set_deployment prod; DEP_DEFAULTED=1; banner "app status") 2>&1)"
if [[ "$banner_out" == *"prod"*"the default for a read-only command"* ]]; then pass "the banner names the defaulted deployment"; else fail "banner: $banner_out"; fi

echo "--- the zone's, the organization's and the CI runners' take no deployment"
expect_run "zone plan" "ran DEP=none DEFAULTED=0" zone plan
expect_run "github apply" "ran DEP=none DEFAULTED=0" github apply
expect_refused "--deployment prod zone apply" "no deployment's" --deployment prod zone apply
expect_refused "--deployment staging github plan" "no deployment's" --deployment staging github plan
expect_run "org apply" "ran DEP=none DEFAULTED=0" org apply
expect_refused "--deployment prod org apply" "no deployment's" --deployment prod org apply
expect_run "ci apply" "ran DEP=none DEFAULTED=0" ci apply
expect_refused "--deployment prod ci apply" "no deployment's" --deployment prod ci apply
expect_refused "the old vault state commands are gone" "unknown command" state pull github

echo "--- prod's changes ask for the typed word"
stdin_is_terminal() { return 1; }
out="$( (set_deployment prod; confirm_prod "roll prod" 0) 2>&1)" && fail "prod with no terminal and no --yes went ahead" \
  || { [[ "$out" == *"without --yes"* ]] && pass "prod refuses with no terminal and no --yes" || fail "prod refusal: $out"; }
( set_deployment prod; confirm_prod "roll prod" 1 ) 2>/dev/null && pass "prod goes ahead with --yes" || fail "prod --yes refused"
( set_deployment staging; confirm_prod "roll staging" 0 ) 2>/dev/null && pass "staging goes ahead unasked" || fail "staging asked"
stdin_is_terminal() { return 0; }
( set_deployment prod; confirm_prod "roll prod" 0 <<<"yes" ) 2>/dev/null && fail "prod took 'yes' for 'prod'" || pass "prod refuses 'yes'"
( set_deployment prod; confirm_prod "roll prod" 0 <<<"prod" ) 2>/dev/null && pass "prod takes the typed word prod" || fail "prod refused 'prod'"
( set_deployment staging; confirm_word staging "destroy staging" 0 <<<"prod" ) 2>/dev/null && fail "staging's destroy took 'prod'" || pass "staging's destroy wants 'staging'"

echo "--- data and images cross one way alone"
aws_auth() { :; }
infra_env_load() { :; }
infra_get() { echo "stub-$1"; }
as_deployment() { echo "as_deployment $*" >&2; return 1; }
out="$( (set_deployment prod; backup_import --from-deployment prod novaworld/x.dump) 2>&1)" \
  && fail "a dump imported into prod from a deployment" \
  || { [[ "$out" == *"staging alone"* ]] && pass "prod imports no deployment's dump" || fail "prod import: $out"; }
out="$( (set_deployment staging; backup_import --from-deployment staging novaworld/x.dump) 2>&1)" \
  && fail "staging imported its own dump as another's" \
  || { [[ "$out" == *"staging alone"* ]] && pass "staging takes prod's dumps alone" || fail "staging import: $out"; }
out="$( (set_deployment staging; backup_import --from-deployment prod novaworld/x.dump) 2>&1)"
[[ "$out" == *"as_deployment prod backup_stream_out novaworld/x.dump"* ]] \
  && pass "staging reads prod's dump as prod (prod's account)" || fail "staging from prod: $out"
release_current_manifest() { echo '{}'; }
out="$( (set_deployment staging; app_deploy --promote prod) 2>&1)" \
  && fail "staging promoted prod's images" \
  || { [[ "$out" == *"into prod alone"* ]] && pass "promotion runs staging into prod alone" || fail "promote: $out"; }
out="$( (set_deployment prod; app_deploy --promote staging --tag x) 2>&1)"
[[ "$out" == *"as_deployment staging release_images_json"* ]] \
  && pass "prod reads staging's release as staging" || fail "prod promote: $out"

echo "--- a deploy never takes app down's zero counts for a first release"
calls="$scratch/calls.log"
ecs_cluster() { echo opennova-staging; }
program_counts() { echo '{"min":1,"desired":1,"max":1}'; }
SSM_DOWN_REPLY=""
aws() {
  case "$1 $2" in
    "ecs describe-services") echo '{"services":[{"taskDefinition":"td:1","desiredCount":0}]}' ;;
    "ssm get-parameter")
      case "$SSM_DOWN_REPLY" in
        down) echo '{"since":"x","counts":{"web":1}}' ;;
        missing) echo "An error occurred (ParameterNotFound) when calling the GetParameter operation:" >&2; return 254 ;;
        *) echo "An error occurred (AccessDeniedException) when calling the GetParameter operation: not authorized" >&2; return 254 ;;
      esac ;;
    "ecs update-service") echo "update-service $*" >>"$calls"; return 1 ;;
    *) echo "aws $*" >>"$calls" ;;
  esac
}
: >"$calls"
SSM_DOWN_REPLY=denied
out="$( (set_deployment staging; service_roll web td:2) 2>&1)" && fail "a deploy went on with the down marker unread" \
  || { if grep -q update-service "$calls"; then fail "an unread down marker started a program: $(cat "$calls")"; \
       elif [[ "$out" == *"whether the programs are down is unknown"* ]]; then pass "an unread down marker stops the deploy before any program starts"; \
       else fail "denied marker: $out"; fi; }
: >"$calls"
SSM_DOWN_REPLY=down
( set_deployment staging; service_roll web td:2 ) >/dev/null 2>&1
if grep -q -- '--desired-count' "$calls"; then fail "app down's zero counts read as a first release: $(cat "$calls")"; else pass "app down's zero counts stay zero"; fi
: >"$calls"
SSM_DOWN_REPLY=missing
( set_deployment staging; service_roll web td:2 ) >/dev/null 2>&1
if grep -q -- '--desired-count 1' "$calls"; then pass "zero counts with no marker are the first release's"; else fail "first release: $(cat "$calls")"; fi
: >"$calls"
SSM_DOWN_REPLY=down
( set_deployment staging; service_roll data td:2 ) >/dev/null 2>&1
if grep -q -- '--desired-count 1' "$calls"; then pass "a program the down record names no count for (added since) starts as a first release"; else fail "a program added since app down: $(cat "$calls")"; fi
unset -f aws

echo "--- hostd is the fleet cluster's daemon: never a count, confirmed on the machines"
# The real roll, down and release functions on a stub aws that records every
# call: hostd's service is the fleet cluster's, its zero count no first
# release, `app down` leaves it running, and a release whose agents never
# report its image points hostd back at the revision it ran.
INFRA[OPENNOVA_FLEET_CLUSTER]=opennova-staging-fleet
current="$scratch/current-td"
HOSTD_RUNNING=2
# The service's revision lives in a file: the stub runs in command substitutions.
aws() {
  echo "aws $*" >>"$calls"
  case "$*" in
    *"services[0].runningCount"*) echo "$HOSTD_RUNNING"; return ;;
    *"services[0].taskDefinition"*) cat "$current"; return ;;
  esac
  case "$1 $2" in
    "ecs describe-services")
      echo '{"services":[{"serviceName":"hostd","taskDefinition":"'"$(cat "$current")"'","desiredCount":0,"runningCount":0,"deployments":[{"id":"d1","status":"PRIMARY","rolloutState":"COMPLETED","desiredCount":0,"runningCount":0,"pendingCount":0}]}]}' ;;
    "ecs update-service")
      local args=("$@") i
      for ((i = 0; i < ${#args[@]}; i++)); do
        [ "${args[$i]}" != --task-definition ] || printf '%s' "${args[$((i + 1))]}" >"$current"
      done
      echo '{"service":{"deployments":[{"id":"d1","status":"PRIMARY"}]}}' ;;
    "ssm get-parameter") echo "An error occurred (ParameterNotFound) when calling the GetParameter operation:" >&2; return 254 ;;
  esac
}
printf 'td:1' >"$current"
: >"$calls"
( set_deployment staging; service_roll hostd td:2 ) >/dev/null 2>&1 || fail "hostd's roll failed: $(cat "$calls")"
if grep -q -- '--desired-count' "$calls"; then fail "hostd's roll passed a count: $(cat "$calls")"
elif grep -q 'ssm get-parameter\|application-autoscaling' "$calls"; then fail "hostd's roll read the down marker or touched its scaling: $(cat "$calls")"
elif grep -q 'update-service --cluster opennova-staging-fleet --service hostd --task-definition td:2' "$calls"; then pass "hostd rolls in the fleet cluster, its zero count no first release"
else fail "hostd's roll: $(cat "$calls")"; fi
: >"$calls"
( set_deployment staging; programs_down ) >/dev/null 2>&1
if grep -Eq -- '--service hostd|/hostd |--services [^-]*hostd' "$calls"; then fail "app down touched hostd: $(grep hostd "$calls")"
elif grep -q 'update-service --cluster opennova-staging --service worker --desired-count 0' "$calls"; then pass "app down stops the programs and leaves hostd running"
else fail "app down: $(cat "$calls")"; fi
# shellcheck disable=SC2317  # the functions under test call both
sleep() { :; }
HOSTD_CONFIRM_TIMEOUT=0
HOSTD_REPORTS=old
# shellcheck disable=SC2317
admin_api_json() {
  echo '{"machines":[{"id":12,"state":"ready","agent_image":"ghcr.io/o/opennova-hostd@sha256:'"$HOSTD_REPORTS"'"},{"id":13,"state":"lost","agent_image":"x"}]}'
}
manifest='{"task_definitions":{"hostd":"td:2"},"images":{"hostd":"ghcr.io/o/opennova-hostd@sha256:new"}}'
printf 'td:1' >"$current"
: >"$calls"
out="$( (set_deployment staging; PROGRAMS=(hostd); release_roll "$manifest") 2>&1)" && fail "a release its agents never reported went on" \
  || { if grep -q 'update-service --cluster opennova-staging-fleet --service hostd --task-definition td:1' "$calls"; \
       then pass "agents that never report the release's image put hostd back on its revision"; else fail "no point-back: $out $(cat "$calls")"; fi; }
HOSTD_REPORTS=new
printf 'td:1' >"$current"
: >"$calls"
( set_deployment staging; PROGRAMS=(hostd); release_roll "$manifest" ) >/dev/null 2>&1 \
  && ! grep -q 'task-definition td:1' "$calls" && pass "every ready machine reporting the image confirms hostd" || fail "confirm: $(cat "$calls")"
HOSTD_REPORTS=old
HOSTD_RUNNING=0
printf 'td:1' >"$current"
: >"$calls"
out="$( (set_deployment staging; PROGRAMS=(hostd); release_roll "$manifest") 2>&1)" \
  && [[ "$out" == *"no machine to confirm"* ]] && pass "with no hostd task there is no machine to confirm" || fail "no machine: $out"
# The exec into web refused: the log names the refused call, and the deploy goes on.
HOSTD_RUNNING=2
# shellcheck disable=SC2317
admin_api_json() {
  echo "An error occurred (AccessDeniedException) when calling the ExecuteCommand operation: User: arn:aws:sts::222222222222:assumed-role/opennova-staging-github-deploy/run is not authorized to perform: ecs:ExecuteCommand" >&2
  return 1
}
printf 'td:1' >"$current"
: >"$calls"
out="$( (set_deployment staging; PROGRAMS=(hostd); release_roll "$manifest") 2>&1)" \
  && [[ "$out" == *"AWS refused the ExecuteCommand call of ECS Exec into a web task"* ]] && ! grep -q 'task-definition td:1' "$calls" \
  && pass "an exec into web refused says so, and the deploy goes on" || fail "exec refused: $out"
unset -f aws sleep admin_api_json


echo "--- app up starts a program the down record names no count for at its desired count"
: >"$calls"
program_counts() { case "$1" in data) echo '{"min":1,"desired":3,"max":3}' ;; *) echo '{"min":1,"desired":1,"max":1}' ;; esac; }
scaling_suspended() { :; }
aws() {
  case "$1 $2" in
    "ssm get-parameter") echo '{"since":"x","counts":{"worker":2,"fleet":1,"web":1,"lobby":1,"gate":1}}' ;;
    "ecs update-service") echo "update-service $*" >>"$calls" ;;
    *) : ;;
  esac
}
( set_deployment staging; programs_up ) >/dev/null 2>&1
if grep -q -- '--service worker --desired-count 2' "$calls" && grep -q -- '--service data --desired-count 3' "$calls"; then
  pass "app up puts the recorded counts back and starts the program added since at its desired count"
else
  fail "app up: $(cat "$calls")"
fi
unset -f aws

echo "--- a rollback refuses a release from before the program split"
release_manifest() {
  echo '{"id":"20261016T000000Z-abc1234","programs":["worker","controller","web","lobby","gate"],"task_definitions":{"controller":"td:5"}}'
}
release_current_manifest() { echo '{"id":"20261019T000000Z-def5678","previous":"20261016T000000Z-abc1234"}'; }
aws_auth() { :; }
infra_env_load() { :; }
infra_get() { echo "stub-$1"; }
out="$( (set_deployment staging; app_rollback --to 20261016T000000Z-abc1234 --yes) 2>&1)" \
  && fail "a rollback to the controller's era went on" \
  || { [[ "$out" == *"before the program split"* ]] && pass "a rollback to a release from before the split is refused" || fail "pre-split rollback: $out"; }
unset -f release_manifest release_current_manifest

echo "--- a release's revisions: the serve image where the policy is read, hostd's where the driver is"
described='{"taskDefinition":{"family":"opennova-staging-x","containerDefinitions":[
  {"name":"worker","environment":[{"name":"ONNET_HOSTED_PROVIDER","value":"ec2"},{"name":"ONNET_HOSTED_DEPLOYMENT","value":"opennova-staging"}],"secrets":[]},
  {"name":"fleet","environment":[{"name":"ONNET_HOSTED_PROVIDER","value":"ec2"}],"secrets":[]},
  {"name":"data","environment":[{"name":"ONNET_OBJECT_STORE_URL","value":"s3://b/data"}],"secrets":[]}]}}'
revision="$(jq -c --argjson images '{"worker":"w@1","fleet":"f@1","data":"d@1"}' --arg serve "serve@1" --arg hostd "hostd@1" \
  --arg hosting 1 --arg dep staging --argjson present '[]' --argjson tags '[]' "$REVISION_JQ" <<<"$described")"
env_of() { jq -c --arg c "$1" '[.containerDefinitions[] | select(.name == $c) | .environment[]? | "\(.name)=\(.value)"]' <<<"$revision"; }
if [[ "$(env_of worker)" == *'"ONNET_HOSTED_HOSTD_IMAGE=hostd@1"'* && "$(env_of worker)" == *'"ONNET_HOSTED_SERVE_IMAGE=serve@1"'* ]] &&
   [[ "$(env_of fleet)" == *'"ONNET_HOSTED_SERVE_IMAGE=serve@1"'* && "$(env_of fleet)" != *HOSTD* ]] &&
   [[ "$(env_of data)" != *SERVE* && "$(env_of data)" != *HOSTD* ]]; then
  pass "the worker gets both images, the fleet the serve image alone, the data program neither"
else
  fail "revision images: worker $(env_of worker) fleet $(env_of fleet) data $(env_of data)"
fi
revision="$(jq -c --argjson images '{"worker":"w@1","fleet":"f@1","data":"d@1"}' --arg serve "" --arg hostd "" \
  --arg hosting 0 --arg dep staging --argjson present '[]' --argjson tags '[]' "$REVISION_JQ" <<<"$described")"
if [[ "$(env_of worker)" == *'"ONNET_HOSTED_PROVIDER=none"'* && "$(env_of fleet)" == *'"ONNET_HOSTED_PROVIDER=none"'* ]]; then
  pass "--no-hosting turns the provider off in every container that reads it"
else
  fail "no hosting: worker $(env_of worker) fleet $(env_of fleet)"
fi

echo "--- fleet logs reads the machines' log group alone"
# The real command (cmd_fleet alone is stubbed above) on a stub aws that records
# every call: CloudWatch reads of the one stream asked for, an EC2 id read as
# its machine-id tag, nothing sent to a machine.
infra_get() { case "$1" in OPENNOVA_MACHINES_LOG_GROUP) echo /opennova/staging/machines ;; *) echo "stub-$1" ;; esac; }
aws() {
  echo "aws $*" >>"$calls"
  case "$1 $2" in
    "ec2 describe-instances")
      case "$*" in
        *i-0aaaaaaaaaaaaaaa1*) echo 12 ;;
        *i-0bbbbbbbbbbbbbbb2*) echo None ;;
        *) echo "An error occurred (InvalidInstanceID.NotFound)" >&2; return 254 ;;
      esac ;;
    "ecs list-container-instances")
      case "$*" in
        *"attribute:machine-id == 12"*) echo arn:aws:ecs:us-east-1:1:container-instance/opennova-staging-fleet/ci12 ;;
        *) echo None ;;
      esac ;;
    "ecs list-tasks") echo arn:aws:ecs:us-east-1:1:task/opennova-staging-fleet/t12 ;;
  esac
}
# expect_logs NAME WANT ARGS...: fleet_logs ARGS makes WANT its last aws call.
expect_logs() {
  local name="$1" want="$2" got
  shift 2
  : >"$calls"
  ( set_deployment staging; fleet_logs "$@" ) >/dev/null 2>&1 || { fail "$name (refused: $( (set_deployment staging; fleet_logs "$@") 2>&1 | tail -n 1))"; return; }
  got="$(tail -n 1 "$calls")"
  if [ "$got" = "$want" ]; then pass "$name"; else fail "$name (got '$got', want '$want')"; fi
}
expect_logs "a slot's stream" \
  "aws logs tail /opennova/staging/machines --log-stream-names 12/slot-3 --since 1h --format short" \
  12 3
expect_logs "hostd's stream (its task on the machine), followed, since 2h" \
  "aws logs tail /opennova/staging/machines --log-stream-names hostd/hostd/t12 --since 2h --format short --follow" \
  12 hostd --since 2h --follow
expect_logs "every stream of the machine, each line naming its stream" \
  "aws logs tail /opennova/staging/machines --log-stream-name-prefix 12/ --since 1h --format detailed" \
  12
expect_logs "an EC2 id read as its machine-id tag" \
  "aws logs tail /opennova/staging/machines --log-stream-names 12/slot-0 --since 1h --format short" \
  i-0aaaaaaaaaaaaaaa1 0
if grep -vq '^aws \(logs tail\|ec2 describe-instances\|ecs list-container-instances\|ecs list-tasks\) ' "$calls"; then fail "fleet logs called more than CloudWatch, EC2's describe and ECS's lists: $(cat "$calls")"; else pass "fleet logs calls CloudWatch's tail, EC2's describe and ECS's lists alone"; fi
for args in "" "x" "0 3" "12 32" "12 03" "12 slot-3" "12 3 4" "12 --tail" "i-0bbbbbbbbbbbbbbb2" "i-0ccccccccccccccc3" "i-XYZ"; do
  : >"$calls"
  # shellcheck disable=SC2086  # the words are the command's arguments
  if ( set_deployment staging; fleet_logs $args ) >/dev/null 2>&1; then
    fail "fleet logs '$args' was taken"
  elif grep -q '^aws logs' "$calls"; then
    fail "fleet logs '$args' read the logs before refusing"
  else
    pass "fleet logs '$args' is refused"
  fi
done
unset -f aws

echo "--- the org stack's first run moves its state before the vault learns of it"
# The real org command (the groups are stubbed above), around stubs of its
# parts, in a scratch directory.
# shellcheck source=deploy/bin/lib/infra.sh
source "$root/deploy/bin/lib/infra.sh"
require_op() { :; }
org_auth() { REGION=us-east-1; }
item_json() { local f="$scratch/item.$1.json"; echo '{"fields":[{"label":"account_id","value":"111111111111"}]}' >"$f"; echo "$f"; }
stack_run_body() { echo "body $*" >>"$calls"; }
VAULT_FAILS_FOR=""
vault_set_fields() {
  echo "vault $*" >>"$calls"
  [ "$1" != "$VAULT_FAILS_FOR" ] || die "stub: writing vault item $1 failed"
}
MIGRATE_FAILS=0
stack_tf() {
  case "$1" in
    init)
      echo "init $*" >>"$calls"
      if [[ " $* " == *" -migrate-state "* ]] && [ "$MIGRATE_FAILS" -eq 1 ]; then return 1; fi ;;
    output)
      case "$*" in
        *deploy_keys*) echo '{"prod":{"access_key_id":"AKIAEXAMPLEEXAMPLE","secret_access_key":"s"},"staging":{"access_key_id":"AKIAEXAMPLEEXAMPLE","secret_access_key":"s"}}' ;;
        *deployments*) echo '{"prod":{"state_bucket":"opennova-tfstate-111111111111","github_deploy_role_arn":"arn:r"},"staging":{"state_bucket":"opennova-tfstate-222222222222","github_deploy_role_arn":"arn:r"}}' ;;
        *sso_portal_url*) echo 'https://d-0.awsapps.com/start' ;;
      esac ;;
  esac
}
ORG_TF_DIR="$scratch/org"
ORG_OVERRIDE="$ORG_TF_DIR/backend_override.tf"
mkdir -p "$ORG_TF_DIR"
: >"$calls"
( cmd_org apply ) >/dev/null 2>&1 || fail "the org stack's first run failed: $(cat "$calls")"
if grep -- '-migrate-state' "$calls" | grep -q -- '-reconfigure'; then
  fail "the state's move passes -reconfigure beside -migrate-state: $(grep -- '-migrate-state' "$calls")"
else
  pass "the state's move inits with -migrate-state -force-copy alone"
fi
migrate_line="$(grep -n -- '-migrate-state' "$calls" | head -n 1 | cut -d: -f1)"
vault_line="$(grep -n '^vault ' "$calls" | head -n 1 | cut -d: -f1)"
if [ -n "$migrate_line" ] && [ -n "$vault_line" ] && [ "$migrate_line" -lt "$vault_line" ]; then
  pass "the vault learns of the bucket only after the state has moved"
else
  fail "write-back before the move: $(cat "$calls")"
fi
if [ "$(grep '^vault ' "$calls" | head -n 1)" = "vault aws-prod state_bucket:STRING" ]; then
  pass "after the move, prod's state_bucket is the first vault write, alone"
else
  fail "the first write after the move: $(grep '^vault ' "$calls" | head -n 1)"
fi
if grep -E '^vault aws-(prod|staging) ' "$calls" | tail -n 1 | grep -q '^vault aws-prod '; then
  pass "the write-back's last deployment write is prod's"
else
  fail "the write-back order: $(grep '^vault ' "$calls")"
fi
[ ! -e "$ORG_TF_DIR/backend_override.tf" ] && pass "the first run's backend override is gone after it" || fail "the backend override stayed"
: >"$calls"
VAULT_FAILS_FOR=aws-staging
( cmd_org apply ) >/dev/null 2>&1 && fail "a failed write-back reported success"
if grep -q '^vault aws-prod state_bucket:STRING$' "$calls"; then
  pass "a write-back failing after the move has already sent the next run to S3 (aws-prod's state_bucket)"
else
  fail "a write-back failing after the move left aws-prod naming no bucket: $(cat "$calls")"
fi
VAULT_FAILS_FOR=""
: >"$calls"
MIGRATE_FAILS=1
( cmd_org apply ) >/dev/null 2>&1 && fail "a failed state move reported success"
if grep -q '^vault ' "$calls"; then
  fail "a failed state move still wrote the vault: $(grep '^vault ' "$calls")"
else
  pass "a failed state move leaves the vault naming no bucket (the next run takes the local state again)"
fi

echo
if [ "$failures" -eq 0 ]; then
  echo "deployment guard: all passed"
else
  echo "deployment guard: $failures failed"
  exit 1
fi
