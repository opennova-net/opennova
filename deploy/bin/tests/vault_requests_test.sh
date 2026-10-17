#!/usr/bin/env bash
# The toolbox's 1Password requests (deploy/bin/on-deploy, "The vault"),
# offline: a run checks the token once, lists the vault once, reads each item
# it needs once (by its id and the vault's, 1Password's cheapest read) into
# the run's cache in the tmpfs, and resolves every field and every deploy/env
# template from that cache (no op read, op inject or op run); a transient
# failure is retried once; an item the run writes is read again; the values
# reach terraform, the AWS CLI and SSM as the vault holds them, and no output
# carries one. It sources on-deploy (which then runs nothing) and stubs op,
# aws, terraform and curl around a fixture vault in a scratch directory, so
# nothing reaches 1Password, AWS or Stripe. Each command's 1Password calls are
# counted and printed (the op stub still answers op read, op inject and op
# run, so the same count runs against an older on-deploy). It needs bash and
# jq; .github/workflows/infra.yml runs it:
#   bash deploy/bin/tests/vault_requests_test.sh
# pass and fail never fail, so `cond && pass || fail` is the if-then-else it reads as.
# shellcheck disable=SC2015
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
export OPENNOVA_ROOT="$root" OPENNOVA_SECRETS_DIR="$scratch/secrets" OP_VAULT=OpenNova-Deploy
# The stub's token: nothing here reaches 1Password.
export OP_SERVICE_ACCOUNT_TOKEN=ops_stub

# shellcheck source=deploy/bin/on-deploy
source "$root/deploy/bin/on-deploy"
# on-deploy's own -e and EXIT trap are its run's, not this check's.
set +e
trap 'rm -rf "$scratch"' EXIT
# No backoff between a failure and its retry here.
VAULT_RETRY_DELAY=0

failures=0
pass() { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1"; failures=$((failures + 1)); }

# ---------------------------------------------------------------------------
# The fixture vault: one JSON file per item, as `op item get --format json`
# answers. Every secret's value carries SECRETVALUE, so a value reaching any
# output is found.
# ---------------------------------------------------------------------------

export STUB_VAULT_DIR="$scratch/vault" STUB_CALLS="$scratch/op.calls" STUB_FAILS="$scratch/op.fails"
export STUB_LIST_REPLY="$scratch/list.reply" STUB_CACHE_MODES="$scratch/cache.modes"
export STUB_TF_ENV="$scratch/tfenv" STUB_TF_CALLS="$scratch/tf.calls" STUB_SSM="$scratch/ssm"
export STUB_VAULT_ID=vltopennovadeploy000000000
mkdir -p "$STUB_VAULT_DIR" "$STUB_TF_ENV" "$STUB_SSM" "$OPENNOVA_SECRETS_DIR"

# mkitem TITLE CATEGORY LABEL=VALUE...: the item, with a 26-character id as
# 1Password's are.
mkitem() {
  local title="$1" category="$2" kv fields='[]' id
  shift 2
  for kv in "$@"; do
    fields="$(jq -c --arg l "${kv%%=*}" --arg v "${kv#*=}" '. + [{id: $l, label: $l, type: "STRING", value: $v}]' <<<"$fields")"
  done
  id="$(printf '%s' "$title" | md5sum | cut -c1-26)"
  jq -n --arg id "$id" --arg t "$title" --arg c "$category" --arg vid "$STUB_VAULT_ID" --argjson f "$fields" \
    '{id: $id, title: $t, category: $c, vault: {id: $vid, name: "OpenNova-Deploy"}, fields: $f}' \
    >"$STUB_VAULT_DIR/$title.json"
}
# The members are JSON over several lines: a value's newlines reach terraform.
MEMBERS_JSON=$'{\n  "maintainer": "admin",\n  "helper": "member"\n}'
STRIPE_PRICES=(price_tier1=price_opennovatier1monthly price_tier2=price_opennovatier2monthly
  price_tier3=price_opennovatier3monthly price_own_machine=price_opennovaownmachinemonthly)
make_vault() {
  rm -f "$STUB_VAULT_DIR"/*.json
  mkitem aws-staging SECURE_NOTE account_id=222222222222 account_name=opennova-staging region=us-east-1 \
    root_email=aws-staging@example.org state_bucket=opennova-tfstate-222222222222 \
    access_key_id=AKIASTAGINGKEY000001 secret_access_key=SECRETVALUEawsstaging \
    github_deploy_role_arn=arn:aws:iam::222222222222:role/opennova-github-deploy
  mkitem aws-prod SECURE_NOTE account_id=111111111111 account_name=opennova-prod region=us-east-1 \
    root_email=aws-prod@example.org state_bucket=opennova-tfstate-111111111111 \
    access_key_id=AKIAPRODKEY000000001 secret_access_key=SECRETVALUEawsprod \
    github_deploy_role_arn=arn:aws:iam::111111111111:role/opennova-github-deploy
  mkitem aws-org API_CREDENTIAL account_id=333333333333 organization_id=o-abcdefghij region=us-east-1 \
    sso_given_name=Ada sso_family_name=Lovelace sso_email=ada@example.org \
    access_key_id=AKIAORGKEY0000000001 secret_access_key=SECRETVALUEawsorg
  mkitem app-staging SERVER admin_api_token=SECRETVALUEadminapistaging admin_basic_auth_user=admin \
    admin_basic_auth_password=SECRETVALUEbasicstaging database_password=SECRETVALUEdbstaging \
    redis_password=SECRETVALUEredisstaging edge_auth_secret=SECRETVALUEedgestaging \
    edge_auth_secret_previous=SECRETVALUEedgepreviousstaging
  mkitem app-prod SERVER edge_auth_secret=SECRETVALUEedgeprod
  mkitem rds-staging SECURE_NOTE master_password=SECRETVALUEmasterstaging
  mkitem cloudflare API_CREDENTIAL api_token=SECRETVALUEcloudflare zone_id=0123456789abcdef0123456789abcdef \
    domain=example.org
  mkitem alerts SECURE_NOTE email=alerts@example.org
  mkitem email SECURE_NOTE forward_to=inbox@example.org
  mkitem stripe-test API_CREDENTIAL secret_key=sk_test_SECRETVALUEstripetest webhook_secret=whsec_SECRETVALUEwebhooktest \
    "${STRIPE_PRICES[@]}" restricted_key=rk_test_SECRETVALUErestricted
  mkitem stripe-live API_CREDENTIAL secret_key=REPLACE_ME webhook_secret=REPLACE_ME price_tier1=REPLACE_ME \
    price_tier2=REPLACE_ME price_tier3=REPLACE_ME price_own_machine=REPLACE_ME
  mkitem github-terraform API_CREDENTIAL token=SECRETVALUEgithubtoken billing_email=billing@example.org \
    "members=$MEMBERS_JSON"
  mkitem reference-assets API_CREDENTIAL token=SECRETVALUEreferenceassets
}
# A fixture field's value.
fixture() { jq -j --arg l "$2" '[.fields[] | select(.label == $l) | .value][0]' "$STUB_VAULT_DIR/$1.json"; }
# Drop a fixture field (an item with $2 empty: the item).
fixture_drop() {
  if [ -z "${2:-}" ]; then rm -f "$STUB_VAULT_DIR/$1.json"; return; fi
  jq --arg l "$2" '.fields |= map(select(.label != $l))' "$STUB_VAULT_DIR/$1.json" >"$scratch/drop.json" \
    && mv "$scratch/drop.json" "$STUB_VAULT_DIR/$1.json"
}
# Set a fixture field.
fixture_set() {
  jq --arg l "$2" --arg v "$3" '.fields |= map(if .label == $l then .value = $v else . end)' \
    "$STUB_VAULT_DIR/$1.json" >"$scratch/set.json" && mv "$scratch/set.json" "$STUB_VAULT_DIR/$1.json"
}

# ---------------------------------------------------------------------------
# The stubs. op logs one line per call to STUB_CALLS; a line KIND:MESSAGE in
# STUB_FAILS makes the next call of KIND (whoami, list, get) fail with
# MESSAGE (op's error form); STUB_LIST_REPLY, when it exists, is the listing.
# ---------------------------------------------------------------------------

# The item file whose id or title is $1.
stub_item_file() {
  local f
  for f in "$STUB_VAULT_DIR"/*.json; do
    [ -e "$f" ] || continue
    if [ "$(jq -r '.id' "$f")" = "$1" ] || [ "$(jq -r '.title' "$f")" = "$1" ]; then
      printf '%s' "$f"
      return 0
    fi
  done
  return 1
}
# A reference's value (op://vault/item/field), as op read answers it.
stub_ref() {
  local path="${1#op://}" item field f
  item="$(cut -d/ -f2 <<<"$path")"
  field="$(cut -d/ -f3 <<<"$path")"
  f="$(stub_item_file "$item")" || { echo "[ERROR] \"$item\" isn't an item in the vault" >&2; return 1; }
  jq -e --arg l "$field" 'any(.fields[]; .label == $l)' "$f" >/dev/null \
    || { echo "[ERROR] \"$field\" isn't a field in \"$item\"" >&2; return 1; }
  jq -j --arg l "$field" '[.fields[] | select(.label == $l) | (.value // "")][0]' "$f"
}
# Whether the next injected failure is for a call of kind $1 (taken off the
# list, and said as op says it).
stub_fail() {
  local next
  [ -s "$STUB_FAILS" ] || return 1
  next="$(head -n 1 "$STUB_FAILS")"
  [ "${next%%:*}" = "$1" ] || return 1
  sed -i '1d' "$STUB_FAILS"
  echo "[ERROR] 2026/10/17 12:00:00 ${next#*:}" >&2
  return 0
}
# A template's references: how many, and how many items they name.
stub_tally() {
  local refs
  refs="$(grep -v '^[[:space:]]*#' "$1" | grep -o 'op://[^[:space:]"#]*')"
  printf 'refs=%s items=%s' "$(grep -c . <<<"$refs")" "$(cut -d/ -f4 <<<"$refs" | sort -u | grep -c .)"
}
op() {
  local sub="${1:-}" verb="" key="" vault="" tpl="" in="" out="" line ref val
  shift || true
  case "$sub" in
    whoami)
      echo whoami >>"$STUB_CALLS"
      ! stub_fail whoami || return 1 ;;
    read)
      [ "${1:-}" != --no-newline ] || shift
      echo "read $1" >>"$STUB_CALLS"
      stub_ref "$1" ;;
    inject)
      while [ $# -gt 0 ]; do
        case "$1" in -i) in="$2"; shift 2 ;; -o) out="$2"; shift 2 ;; *) shift ;; esac
      done
      echo "inject $(stub_tally "$in")" >>"$STUB_CALLS"
      while IFS= read -r line || [ -n "$line" ]; do
        while [[ "$line" =~ (op://[^[:space:]\"#]+) ]]; do
          ref="${BASH_REMATCH[1]}"
          val="$(stub_ref "$ref")" || return 1
          line="${line/"$ref"/"$val"}"
        done
        printf '%s\n' "$line"
      done <"$in" >"$out" ;;
    run)
      in="${1#--env-file=}"
      shift 2
      echo "run $(stub_tally "$in")" >>"$STUB_CALLS"
      (
        while IFS= read -r line || [ -n "$line" ]; do
          [[ "$line" =~ ^([A-Z_][A-Z0-9_]*)=(op://[^[:space:]]+) ]] || continue
          val="$(stub_ref "${BASH_REMATCH[2]}")" || exit 1
          export "${BASH_REMATCH[1]}=$val"
        done <"$in"
        "$@"
      ) ;;
    item)
      verb="${1:-}"
      shift || true
      while [ $# -gt 0 ]; do
        case "$1" in
          --vault) vault="$2"; shift 2 ;;
          --template) tpl="$2"; shift 2 ;;
          --format) shift 2 ;;
          --*) shift ;;
          *) key="$1"; shift ;;
        esac
      done
      case "$verb" in
        list)
          echo "list vault=$vault" >>"$STUB_CALLS"
          ! stub_fail list || return 1
          if [ -e "$STUB_LIST_REPLY" ]; then cat "$STUB_LIST_REPLY"; return 0; fi
          jq -s '[.[] | {id, title, version: 1, vault, category}]' "$STUB_VAULT_DIR"/*.json ;;
        get)
          echo "get $key vault=$vault" >>"$STUB_CALLS"
          ! stub_fail get || return 1
          out="$(stub_item_file "$key")" || { echo "[ERROR] \"$key\" isn't an item in the \"$vault\" vault" >&2; return 1; }
          cat "$out" ;;
        edit)
          echo "edit $key vault=$vault" >>"$STUB_CALLS"
          out="$(stub_item_file "$key")" || { echo "[ERROR] \"$key\" isn't an item" >&2; return 1; }
          jq --slurpfile t "$tpl" '.fields = $t[0].fields' "$out" >"$out.new" && mv "$out.new" "$out" ;;
        create)
          echo "create vault=$vault" >>"$STUB_CALLS"
          key="$(jq -r '.title' "$tpl")"
          jq --arg id "$(printf '%s' "$key" | md5sum | cut -c1-26)" --arg vid "$STUB_VAULT_ID" \
            '. + {id: $id, vault: {id: $vid, name: "OpenNova-Deploy"}}' "$tpl" >"$STUB_VAULT_DIR/$key.json" ;;
        *) echo "item $verb" >>"$STUB_CALLS"; return 1 ;;
      esac ;;
    *) echo "$sub" >>"$STUB_CALLS"; return 1 ;;
  esac
}

# aws: the account each fixture key acts in, infra.env, SSM's secrets (each
# value put lands in STUB_SSM/<NAME>, never on an output).
aws() {
  local name="" value="" a
  case "${1:-} ${2:-}" in
    "sts get-caller-identity")
      case "${AWS_ACCESS_KEY_ID:-}" in
        AKIASTAGING*) a=222222222222 ;;
        AKIAPROD*) a=111111111111 ;;
        AKIAORG*) a=333333333333 ;;
        *) echo "Unable to locate credentials" >&2; return 253 ;;
      esac
      printf '{"Account":"%s","Arn":"arn:aws:iam::%s:user/opennova-deploy"}\n' "$a" "$a" ;;
    "ssm get-parameter")
      while [ $# -gt 0 ]; do case "$1" in --name) name="$2"; shift 2 ;; *) shift ;; esac; done
      case "$name" in
        */infra.env) printf "OPENNOVA_DEPLOYMENT='%s'\nOPENNOVA_CLUSTER='opennova-%s'\nOPENNOVA_LOG_GROUP='/opennova/%s/service'\n" "$DEP" "$DEP" "$DEP" ;;
        */release) echo none ;;
        *) echo "An error occurred (ParameterNotFound)" >&2; return 254 ;;
      esac ;;
    "ssm put-parameter")
      while [ $# -gt 0 ]; do
        case "$1" in --name) name="$2"; shift 2 ;; --value) value="$2"; shift 2 ;; *) shift ;; esac
      done
      cp "${value#file://}" "$STUB_SSM/${name##*/}" ;;
    "ssm get-parameters-by-path") echo "" ;;
    "ecs describe-services") echo '{"services":[]}' ;;
    *) : ;;
  esac
}

# terraform: each variable a run sees from the vault lands in
# STUB_TF_ENV/<NAME> (never on an output), and the run's vault cache's modes
# in STUB_CACHE_MODES; show and output answer as an empty plan and the org
# stack's outputs do.
terraform() {
  local v
  echo "terraform $*" >>"$STUB_TF_CALLS"
  case " $* " in
    *" plan "*|*" apply "*|*" import "*|*" destroy "*)
      rm -f "${STUB_TF_ENV:?}"/*
      for v in $(compgen -e); do
        case "$v" in TF_VAR_*|GITHUB_TOKEN|AWS_ACCESS_KEY_ID|AWS_SECRET_ACCESS_KEY|TF_DATA_DIR) printf '%s' "${!v}" >"$STUB_TF_ENV/$v" ;; esac
      done
      [ ! -d "${VAULT_CACHE:-}" ] || stat -c '%a %n' "$VAULT_CACHE" "$VAULT_CACHE"/* >"$STUB_CACHE_MODES" ;;
  esac
  case " $* " in
    *" show -json "*) echo '{"resource_changes":[]}' ;;
    *" output -json deployments "*) echo '{"prod":{"state_bucket":"opennova-tfstate-111111111111","github_deploy_role_arn":"arn:r:prod"},"staging":{"state_bucket":"opennova-tfstate-222222222222","github_deploy_role_arn":"arn:r:staging"}}' ;;
    *" output -json deploy_keys "*) echo '{"prod":{"access_key_id":"AKIAPRODKEY000000001","secret_access_key":"SECRETVALUEawsprod"},"staging":{"access_key_id":"AKIASTAGINGKEY000001","secret_access_key":"SECRETVALUEawsstaging2"}}' ;;
    *" output -raw sso_portal_url "*) printf 'https://d-0000000000.awsapps.com/start' ;;
  esac
  return 0
}

# curl: Stripe's answers for a complete test-mode setup, to the key the vault
# holds alone (the one curl's config file must carry).
curl() {
  local out="" url="" cfg="" lookup="" a events
  while [ $# -gt 0 ]; do
    case "$1" in
      -o) out="$2"; shift 2 ;;
      -K) cfg="$2"; shift 2 ;;
      -w|--max-time|-X) shift 2 ;;
      --data-urlencode) case "$2" in "lookup_keys[]="*) lookup="${2#*=}" ;; esac; shift 2 ;;
      -*) shift ;;
      *) url="$1"; shift ;;
    esac
  done
  if ! grep -q "Bearer $(fixture stripe-test secret_key)\"" "$cfg"; then
    echo '{"error":{"type":"invalid_request_error","message":"Invalid API Key provided"}}' >"$out"
    printf 401
    return 0
  fi
  events="$(printf '%s\n' "${STRIPE_EVENTS[@]}" | jq -R . | jq -sc .)"
  case "$url" in
    */v1/prices/*)
      printf '{"id":"%s","active":true,"type":"recurring","recurring":{"interval":"month","interval_count":1},"livemode":false,"unit_amount":500}' "${url##*/}" >"$out" ;;
    */v1/prices) printf '{"data":[{"id":"price_%s"}]}' "${lookup//_/}" >"$out" ;;
    */v1/billing_portal/configurations) echo '{"data":[{"id":"bpc_1","metadata":{"opennova":"support_tiers"},"is_default":true}]}' >"$out" ;;
    */v1/webhook_endpoints)
      a="https://staging.$(fixture cloudflare domain)/api/billing/webhook"
      jq -n --arg u "$a" --arg v "$STRIPE_API_VERSION" --argjson e "$events" \
        '{data: [{id: "we_1", url: $u, status: "enabled", api_version: $v, enabled_events: $e}]}' >"$out" ;;
    *) echo '{}' >"$out" ;;
  esac
  printf 200
}

# ---------------------------------------------------------------------------
# Counting: one command, a fresh run (an empty cache), its calls tallied
# ---------------------------------------------------------------------------

# A fresh run's state: the fixture vault, no cache, no calls, no failures.
fresh_run() {
  make_vault
  rm -rf "${VAULT_CACHE:-$scratch/no-cache}" "$OPENNOVA_SECRETS_DIR"/item.* "${STUB_TF_ENV:?}"/* "${STUB_SSM:?}"/* \
    "$STUB_LIST_REPLY" "$STUB_CACHE_MODES"
  : >"$STUB_CALLS"
  : >"$STUB_FAILS"
  : >"$STUB_TF_CALLS"
}

# The calls of STUB_CALLS as one line: each kind's count, and the read
# requests they cost by 1Password's documented counts (developer docs,
# "Use service accounts with 1Password CLI"): op read and op item get by
# name 3, op item get by item and vault id 1, op item list --vault NAME 3;
# op inject and op run are undocumented, so a range: 3 a named item (each
# item resolved once) up to 3 a reference (each reference resolved alone).
# op whoami and the writes are counted apart (their costs are undocumented).
tally() {
  local refs items min max
  TALLY_CALLS="$(grep -c . "$STUB_CALLS")"
  TALLY_WHOAMI="$(grep -c '^whoami$' "$STUB_CALLS")"
  TALLY_LISTS="$(grep -c '^list ' "$STUB_CALLS")"
  TALLY_GETS="$(grep -c '^get ' "$STUB_CALLS")"
  TALLY_GETS_BY_ID="$(grep -cE "^get [0-9a-f]{26} vault=$STUB_VAULT_ID\$" "$STUB_CALLS")"
  TALLY_READS="$(grep -c '^read ' "$STUB_CALLS")"
  TALLY_INJECTS="$(grep -cE '^(inject|run) ' "$STUB_CALLS")"
  TALLY_WRITES="$(grep -cE '^(edit|create) ' "$STUB_CALLS")"
  refs="$(grep -oE '^(inject|run) refs=[0-9]+' "$STUB_CALLS" | grep -oE '[0-9]+$' | awk '{ s += $1 } END { print s + 0 }')"
  items="$(grep -oE 'items=[0-9]+' "$STUB_CALLS" | grep -oE '[0-9]+$' | awk '{ s += $1 } END { print s + 0 }')"
  min=$((3 * TALLY_LISTS + 3 * (TALLY_GETS - TALLY_GETS_BY_ID) + TALLY_GETS_BY_ID + 3 * TALLY_READS + 3 * items))
  max=$((3 * TALLY_LISTS + 3 * (TALLY_GETS - TALLY_GETS_BY_ID) + TALLY_GETS_BY_ID + 3 * TALLY_READS + 3 * refs))
  if [ "$min" -eq "$max" ]; then TALLY_RANGE="$min"; else TALLY_RANGE="$min..$max"; fi
  printf 'op calls %2d (whoami %d, item list %d, item get %d, read %d, inject/run %d%s, edit/create %d); read requests %s\n' \
    "$TALLY_CALLS" "$TALLY_WHOAMI" "$TALLY_LISTS" "$TALLY_GETS" "$TALLY_READS" "$TALLY_INJECTS" \
    "$([ "$TALLY_INJECTS" -eq 0 ] || printf ': %s refs in %s items' "$refs" "$items")" "$TALLY_WRITES" "$TALLY_RANGE"
}

# run_main ARGS...: main ARGS in a fresh run (fresh_run first, or the caller's
# own setup with RUN_KEEP=1); RUN_RC its status, RUN_OUT its output (stdout
# and stderr together), then tallied (tally's TALLY_*).
run_main() {
  [ "${RUN_KEEP:-0}" -eq 1 ] || fresh_run
  RUN_RC=0
  RUN_OUT="$( (main "$@") </dev/null 2>&1)" || RUN_RC=$?
  tally >/dev/null
}

# No output names a secret's value.
no_secret_in() {
  if [[ "$2" == *SECRETVALUE* ]]; then
    fail "$1 printed a secret: $(grep -o '[^ ]*SECRETVALUE[^ ]*' <<<"$2" | head -n 3 | tr '\n' ' ')"
  else
    pass "$1 printed no secret"
  fi
}

# expect_requests NAME GETS ARGS...: main ARGS succeeds with one token check,
# one listing, GETS item reads (each by its ids, none twice) and no op read,
# op inject or op run; its line of the table is printed.
expect_requests() {
  local name="$1" gets="$2" twice
  shift 2
  run_main "$@"
  printf '  %-44s %s\n' "$name" "$(tally)"
  [ "$RUN_RC" -eq 0 ] || { fail "$name exited $RUN_RC: $(tail -n 3 <<<"$RUN_OUT")"; return; }
  twice="$(grep '^get ' "$STUB_CALLS" | sort | uniq -d | tr '\n' ' ')"
  if [ "$TALLY_WHOAMI" -eq 1 ] && [ "$TALLY_LISTS" -eq 1 ] && [ "$TALLY_GETS" -eq "$gets" ] \
     && [ "$TALLY_GETS_BY_ID" -eq "$gets" ] && [ "$TALLY_READS" -eq 0 ] && [ "$TALLY_INJECTS" -eq 0 ] \
     && [ -z "$twice" ]; then
    pass "$name: 1 token check, 1 listing, $gets item reads by id"
  else
    fail "$name: $(tally)${twice:+; read twice: $twice}"
  fi
  no_secret_in "$name" "$RUN_OUT"
}

# The value terraform's run saw for $1 is fixture $2/$3's.
expect_tf_var() {
  local got want
  got="$(cat "$STUB_TF_ENV/$1" 2>/dev/null)" || { fail "terraform saw no $1"; return; }
  want="$(fixture "$2" "$3")"
  [ "$got" = "$want" ] && pass "terraform's $1 is $2/$3" || fail "terraform's $1 is not $2/$3"
}

echo "--- each command's 1Password calls (a fresh run: one token check, one listing, each item once)"
expect_requests "--deployment staging app status" 1 --deployment staging app status
expect_requests "--deployment staging aws sts get-caller-identity" 1 --deployment staging aws sts get-caller-identity
expect_requests "--deployment staging infra plan" 6 --deployment staging infra plan
for v in cloudflare_api_token:cloudflare:api_token cloudflare_zone_id:cloudflare:zone_id \
         cloudflare_domain:cloudflare:domain db_master_password:rds-staging:master_password \
         cache_auth_token:app-staging:redis_password alert_email:alerts:email \
         email_forward_to:email:forward_to; do
  IFS=: read -r name item field <<<"$v"
  expect_tf_var "TF_VAR_$name" "$item" "$field"
done
expect_tf_var AWS_ACCESS_KEY_ID aws-staging access_key_id
expect_tf_var AWS_SECRET_ACCESS_KEY aws-staging secret_access_key
[ "$(cat "$STUB_TF_ENV/TF_DATA_DIR" 2>/dev/null)" = "$root/infra/aws/.terraform-staging" ] \
  && pass "terraform runs in the deployment's data directory" || fail "TF_DATA_DIR: $(cat "$STUB_TF_ENV/TF_DATA_DIR" 2>/dev/null)"
if grep -vE '^(700|600) ' "$STUB_CACHE_MODES" | grep -q . || ! grep -q '^700 ' "$STUB_CACHE_MODES"; then
  fail "the vault cache is not 0700 with 0600 files: $(tr '\n' ' ' <"$STUB_CACHE_MODES")"
else
  pass "the vault cache is 0700, its files 0600"
fi
expect_requests "--deployment staging infra apply" 6 --deployment staging infra apply
[ "$(grep -cE -- ' (plan|apply) ' "$STUB_TF_CALLS")" -eq 2 ] \
  && pass "infra apply's plan and apply both run on the one read of each item" || fail "infra apply's terraform runs: $(cat "$STUB_TF_CALLS")"
expect_tf_var TF_VAR_db_master_password rds-staging master_password
expect_requests "--deployment staging secrets check" 9 --deployment staging secrets check
expect_requests "--deployment staging secrets sync" 3 --deployment staging secrets sync
matched=0
while IFS= read -r line; do
  parse_runtime_line "$line" || continue
  ref="${RT_REF//@VAULT@/OpenNova-Deploy}"
  ref="${ref//@APP_ITEM@/app-staging}"
  ref="${ref//@STRIPE_ITEM@/stripe-test}"
  item="$(cut -d/ -f4 <<<"$ref")" field="$(cut -d/ -f5 <<<"$ref")"
  if jq -e --arg l "$field" 'any(.fields[]; .label == $l)' "$STUB_VAULT_DIR/$item.json" >/dev/null; then
    if [ "$(cat "$STUB_SSM/$RT_NAME" 2>/dev/null)" = "$(fixture "$item" "$field")" ]; then matched=$((matched + 1)); else fail "SSM's $RT_NAME is not $item/$field"; fi
  elif [ -e "$STUB_SSM/$RT_NAME" ]; then
    fail "SSM got $RT_NAME, whose optional field $item/$field is absent"
  fi
done <"$RUNTIME_TPL"
[ "$matched" -eq 13 ] && pass "secrets sync wrote each of the 13 secrets the vault holds, as it holds them" \
  || fail "secrets sync wrote $matched of the 13 secrets as the vault holds them ($(cd "$STUB_SSM" && printf '%s ' *))"
expect_requests "--deployment staging stripe check" 2 --deployment staging stripe check
expect_requests "org plan" 3 org plan
for v in region:aws-org:region organization_id:aws-org:organization_id management_account_id:aws-org:account_id \
         aws_account_id_prod:aws-prod:account_id aws_account_id_staging:aws-staging:account_id \
         account_name_prod:aws-prod:account_name account_name_staging:aws-staging:account_name \
         root_email_prod:aws-prod:root_email root_email_staging:aws-staging:root_email \
         sso_given_name:aws-org:sso_given_name sso_family_name:aws-org:sso_family_name sso_email:aws-org:sso_email; do
  IFS=: read -r name item field <<<"$v"
  expect_tf_var "TF_VAR_$name" "$item" "$field"
done
expect_tf_var AWS_ACCESS_KEY_ID aws-org access_key_id
expect_requests "zone plan" 5 zone plan
expect_tf_var TF_VAR_edge_auth_secret_prod app-prod edge_auth_secret
expect_tf_var TF_VAR_edge_auth_secret_staging app-staging edge_auth_secret
expect_tf_var AWS_ACCESS_KEY_ID aws-prod access_key_id
expect_requests "github plan" 5 github plan
expect_tf_var GITHUB_TOKEN github-terraform token
expect_tf_var TF_VAR_members github-terraform members
expect_tf_var TF_VAR_stripe_test_secret_key stripe-test restricted_key
expect_tf_var TF_VAR_aws_region_staging aws-staging region

fresh_run
jq '.title = "Cloudflare" | .fields |= map(if .label == "api_token" then .label = "API_Token" | .id = "f0123456789" else . end)' \
  "$STUB_VAULT_DIR/cloudflare.json" >"$scratch/cf.json" && mv "$scratch/cf.json" "$STUB_VAULT_DIR/cloudflare.json"
RUN_KEEP=1 run_main --deployment staging infra plan
[ "$RUN_RC" -eq 0 ] && [ "$(cat "$STUB_TF_ENV/TF_VAR_cloudflare_api_token" 2>/dev/null)" = SECRETVALUEcloudflare ] \
  && pass "a reference finds its item and field in any case, as op does" || fail "case: rc $RUN_RC: $(tail -n 2 <<<"$RUN_OUT")"

echo "--- a write reads its item fresh, and the run reads it again after"
run_main org apply --yes
printf '  %-44s %s\n' "org apply --yes" "$(tally)"
[ "$RUN_RC" -eq 0 ] || fail "org apply exited $RUN_RC: $(tail -n 3 <<<"$RUN_OUT")"
twice="$(grep '^get ' "$STUB_CALLS" | sort | uniq -c | awk '$1 > 2 { print $3 }' | tr '\n' ' ')"
if [ "$TALLY_WHOAMI" -eq 1 ] && [ "$TALLY_LISTS" -eq 1 ] && [ "$TALLY_GETS" -eq 6 ] && [ "$TALLY_WRITES" -eq 3 ] \
   && [ "$TALLY_READS" -eq 0 ] && [ "$TALLY_INJECTS" -eq 0 ] && [ -z "$twice" ]; then
  pass "org apply: each item read once to plan, once more fresh before its write"
else
  fail "org apply: $(tally)"
fi
[ "$(fixture aws-staging secret_access_key)" = SECRETVALUEawsstaging2 ] && [ "$(fixture aws-org sso_portal_url)" = https://d-0000000000.awsapps.com/start ] \
  && pass "org apply's write-back reached the vault" || fail "org apply's write-back is not in the vault"
no_secret_in "org apply" "$RUN_OUT"
fresh_run
out="$( (
  set_deployment staging
  vault_value stripe-test price_tier1 && printf 'before=%s\n' "$VAULT_VALUE"
  VAULT_VALUES[price_tier1]=price_written1tier
  vault_set_fields stripe-test price_tier1:STRING
  vault_value stripe-test price_tier1 && printf 'after=%s\n' "$VAULT_VALUE"
) 2>&1)"
[ "$out" = $'before=price_opennovatier1monthly\nafter=price_written1tier' ] \
  && pass "a field written in a run reads back written in that run" || fail "read after write: $out"
[ "$(grep -c "^get $(printf '%s' stripe-test | md5sum | cut -c1-26) " "$STUB_CALLS")" -eq 3 ] \
  && pass "the write reads its item fresh before, and once after" || fail "the write's reads: $(tr '\n' ';' <"$STUB_CALLS")"
fresh_run
out="$( (
  set_deployment staging
  ensure_item app-staging SERVER admin_api_token:gen:24:CONCEALED bootstrap_admin:value:REPLACE_ME:STRING
  ensure_item notes-prod SECURE_NOTE note:value:kept:STRING
  vault_has_item notes-prod && echo "listed notes-prod"
) 2>&1)"
if [ "$(fixture app-staging admin_api_token)" = SECRETVALUEadminapistaging ] && [ "$(fixture app-staging bootstrap_admin)" = REPLACE_ME ] \
   && [ -e "$STUB_VAULT_DIR/notes-prod.json" ] && [[ "$out" == *"listed notes-prod"* ]]; then
  pass "secrets init adds a missing field, keeps the others, and a created item is listed after"
else
  fail "ensure_item: $out"
fi
no_secret_in "secrets init" "$out"

echo "--- a transient failure is retried once; a lasting one names the item"
fresh_run
echo "get:(500) Internal Server Error" >"$STUB_FAILS"
RUN_KEEP=1 run_main --deployment staging app status
[ "$RUN_RC" -eq 0 ] && [ "$TALLY_GETS" -eq 2 ] && [[ "$RUN_OUT" == *"trying once more"* ]] \
  && pass "a 500 on an item read is retried once" || fail "500 retry: rc $RUN_RC, $(tally): $RUN_OUT"
fresh_run
echo "list:(400) The structure of request was invalid" >"$STUB_FAILS"
RUN_KEEP=1 run_main --deployment staging app status
[ "$RUN_RC" -eq 0 ] && [ "$TALLY_LISTS" -eq 2 ] \
  && pass "a malformed-request answer on the listing is retried once" || fail "400 retry: rc $RUN_RC, $(tally): $RUN_OUT"
fresh_run
printf '%s\n' "get:Too many requests. Your client has been rate-limited." "get:Too many requests. Your client has been rate-limited." >"$STUB_FAILS"
RUN_KEEP=1 run_main --deployment staging app status
[ "$RUN_RC" -ne 0 ] && [ "$TALLY_GETS" -eq 2 ] && [[ "$RUN_OUT" == *"reading vault item 'aws-staging' failed"*"rate-limited"* ]] \
  && pass "a rate limit that lasts stops the run after one retry, naming the item" || fail "lasting rate limit: rc $RUN_RC, $(tally): $RUN_OUT"
fresh_run
echo "get:(403) Forbidden" >"$STUB_FAILS"
RUN_KEEP=1 run_main --deployment staging app status
[ "$RUN_RC" -ne 0 ] && [ "$TALLY_GETS" -eq 1 ] \
  && pass "a refusal is not retried" || fail "403: rc $RUN_RC, $(tally): $RUN_OUT"
fresh_run
echo "whoami:(401) Unauthorized" >"$STUB_FAILS"
RUN_KEEP=1 run_main --deployment staging app status
[ "$RUN_RC" -ne 0 ] && [[ "$RUN_OUT" == *"1Password auth failed (check OP_SERVICE_ACCOUNT_TOKEN)"* ]] && [ "$TALLY_LISTS" -eq 0 ] \
  && pass "a token 1Password refuses stops the run first" || fail "auth: rc $RUN_RC, $(tally): $RUN_OUT"

echo "--- what the vault lacks stops a run with its hint, and no value"
# expect_stop NAME TEXT ARGS...: main ARGS fails, naming TEXT, printing no secret.
expect_stop() {
  local name="$1" text="$2"
  shift 2
  RUN_KEEP=1 run_main "$@"
  if [ "$RUN_RC" -eq 0 ]; then
    fail "$name went on"
  elif [[ "$RUN_OUT" != *"$text"* ]]; then
    fail "$name stopped, but not naming '$text': $RUN_OUT"
  elif [[ "$RUN_OUT" == *SECRETVALUE* ]]; then
    fail "$name printed a secret"
  else
    pass "$name"
  fi
}
fresh_run
fixture_drop cloudflare zone_id
expect_stop "a template's missing field stops terraform before it runs" \
  "terraform.env.tpl: TF_VAR_cloudflare_zone_id (op://OpenNova-Deploy/cloudflare/zone_id): vault item 'cloudflare' has no field 'zone_id'. Nothing was run (secrets check names what the vault lacks; secrets init adds" \
  --deployment staging infra plan
grep -q ' plan ' "$STUB_TF_CALLS" && fail "terraform planned with a field missing" || pass "terraform did not plan with a field missing"
fresh_run
fixture_drop rds-staging
expect_stop "a template's missing item names it" "vault 'OpenNova-Deploy' holds no item 'rds-staging'" --deployment staging infra plan
fresh_run
fixture_drop aws-staging
expect_stop "the deployment's absent aws item asks for secrets init" \
  "vault item 'aws-staging' is absent: ./deploy/run.sh --deployment staging secrets init makes it" --deployment staging app status
fresh_run
fixture_drop app-staging database_password
expect_stop "secrets sync stops at a missing secret" \
  "reading DATABASE_PASSWORD (op://OpenNova-Deploy/app-staging/database_password) failed" --deployment staging secrets sync
fresh_run
fixture_drop app-staging database_password
expect_stop "secrets check fails a missing secret" "DATABASE_PASSWORD    FAILED (op://OpenNova-Deploy/app-staging/database_password)" \
  --deployment staging secrets check
fresh_run
fixture_drop rds-staging
out="$( (set_deployment staging; db_master_param_put) 2>&1)"
[[ "$out" == *"reading op://OpenNova-Deploy/rds-staging/master_password failed (secrets init makes it)"* ]] \
  && pass "db's master password names its item and secrets init" || fail "db master password: $out"
fresh_run
fixture_drop aws-staging access_key_id
expect_stop "an item with no access key asks for org apply" "aws-staging holds no access key" --deployment staging app status
fresh_run
: >"$STUB_LIST_REPLY"
expect_stop "an empty listing stops the run" "listing vault 'OpenNova-Deploy' came back empty" --deployment staging app status
fresh_run
echo '{"items":[]}' >"$STUB_LIST_REPLY"
expect_stop "a listing that is no array stops the run" "vault 'OpenNova-Deploy' listing unreadable" --deployment staging app status

echo "--- the deployment guard holds with the cache"
fresh_run
fixture_set aws-staging access_key_id AKIAPRODKEY000000001
expect_stop "credentials of another account are refused" \
  "the AWS credentials act in account 111111111111, not staging's (222222222222, vault item aws-staging). Refusing" \
  --deployment staging app status
fresh_run
out="$( (
  set_deployment prod
  aws_auth 2>/dev/null
  as_deployment staging printenv AWS_ACCESS_KEY_ID 2>/dev/null
  printenv AWS_ACCESS_KEY_ID
) )"
[ "$out" = $'AKIASTAGINGKEY000001\nAKIAPRODKEY000000001' ] \
  && pass "a one-way read runs as the other deployment in its own subshell, and the run stays its own" \
  || fail "as_deployment: $out"
tally >/dev/null
[ "$TALLY_WHOAMI" -eq 1 ] && [ "$TALLY_LISTS" -eq 1 ] && [ "$TALLY_GETS" -eq 2 ] \
  && pass "the one-way read shares the run's token check and listing" || fail "as_deployment's calls: $(tally)"
fresh_run
( set_deployment staging; aws_auth 2>/dev/null; tf_op plan >/dev/null 2>&1; [ -z "${TF_VAR_cloudflare_api_token:-}${TF_VAR_db_master_password:-}" ] ) \
  && pass "a template's variables stay in terraform's environment alone" || fail "a template's variables leaked into the toolbox's shell"

echo "--- the cache lives in the run's tmpfs and goes with it"
fresh_run
export -f op aws terraform curl stub_item_file stub_ref stub_fail stub_tally fixture
out="$(bash "$root/deploy/bin/on-deploy" --deployment staging infra plan </dev/null 2>&1)"
rc=$?
[ "$rc" -eq 0 ] || fail "on-deploy as a program: exit $rc: $(tail -n 3 <<<"$out")"
grep -q '/vault\.[0-9]*$' "$STUB_CACHE_MODES" && pass "the run's cache is in its secrets directory" \
  || fail "the cache's place: $(tr '\n' ' ' <"$STUB_CACHE_MODES")"
leftover="$(find "$OPENNOVA_SECRETS_DIR" -mindepth 1 2>/dev/null | tr '\n' ' ')"
[ -z "$leftover" ] && pass "nothing is left in the secrets directory when the run ends" || fail "left in the tmpfs: $leftover"
export -n -f op aws terraform curl stub_item_file stub_ref stub_fail stub_tally fixture

echo
if [ "$failures" -eq 0 ]; then
  echo "vault requests: all passed"
else
  echo "vault requests: $failures failed"
  exit 1
fi
