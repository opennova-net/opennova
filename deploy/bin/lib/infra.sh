# shellcheck shell=bash
# on-deploy's Terraform: the deployment's stack (`infra`), the Organization's
# (`org`), the Cloudflare zone's (`zone`) and the GitHub organization's
# (`github`). Every state lives in an S3 bucket of a deployment's account
# (prod's for the three shared ones), locked by S3's own lock file.
# deploy/bin/on-deploy sources this file before it runs a command: it defines
# functions and their constants alone, and runs nothing itself. The helpers
# every command shares (log, die, the deployment, the vault, AWS) are
# on-deploy's own.

# ---------------------------------------------------------------------------
# infra
# ---------------------------------------------------------------------------

# The data sets hosted game servers run: each the games slug of the lobby its
# servers list in (apps/novaworld_common/hosted, the service's data-set
# catalog), one snapshot each (infra/aws/fleet.tf).
HOSTED_DATA_SETS=(jop_2_consumer dfx2_consumer opennova_0_consumer)

# Whether $1 is one of HOSTED_DATA_SETS.
hosted_known_data_set() {
  local slug
  for slug in "${HOSTED_DATA_SETS[@]}"; do
    [ "$slug" = "$1" ] && return 0
  done
  return 1
}

# The deployment's data sets' snapshots in its vault item (hosted-<deployment>,
# one field per data set, data_snapshot_id_<slug>: snapshots in the
# deployment's own account), as a JSON object slug -> snapshot id on stdout.
# The item is read once (the run's cache); a failed read, a field naming no
# data set, a value that is no snapshot id, or an item naming no data set at
# all fails (the reason on stderr), so nothing reads a broken item as hosting
# off. Run it in a command substitution and check its status.
hosted_snapshot_map() {
  local fields label value slug map="{}"
  vault_item_load "$HOSTED_ITEM" \
    || die "reading vault item '$HOSTED_ITEM' failed (fix the vault access, or delete the item to turn $DEP's hosting off): $VAULT_ERR"
  fields="$(jq -r '.fields[]? | select((.label // "") | startswith("data_snapshot_id")) | [.label, (.value // "")] | @tsv' "$(vault_item_file "$HOSTED_ITEM")")" \
    || die "vault item '$HOSTED_ITEM' came back unreadable (no JSON with fields)"
  while IFS=$'\t' read -r label value; do
    [ -n "$label" ] || continue
    slug="${label#data_snapshot_id_}"
    hosted_known_data_set "$slug" \
      || die "vault item '$HOSTED_ITEM' has the field $label, which names no data set (data_snapshot_id_<slug>, the slug one of: ${HOSTED_DATA_SETS[*]})"
    case "$value" in
      snap-*) ;;
      *) die "vault item '$HOSTED_ITEM' field $label holds '$value', which is no snapshot id (snap-...)" ;;
    esac
    map="$(jq -c --arg k "$slug" --arg v "$value" '. + {($k): $v}' <<<"$map")"
  done <<<"$fields"
  [ "$map" != "{}" ] \
    || die "vault item '$HOSTED_ITEM' names no data set: add a field data_snapshot_id_<slug> per data set (the slug one of: ${HOSTED_DATA_SETS[*]}), or delete the item to turn $DEP's hosting off"
  printf '%s' "$map"
}

# The hosted game servers' data snapshots (infra/aws/fleet.tf), from the
# deployment's vault item as TF_VAR_hosted_data_snapshots (a JSON object, which
# Terraform reads as the map). No such item in the vault's listing means the
# deployment's hosting is off (the map stays empty, and fleet.tf makes
# nothing). Any other failure (the listing, or reading an item that is there)
# stops the run, so a vault hiccup never plans the hosting resources away.
hosted_snapshot_var() {
  local map
  if ! vault_has_item "$HOSTED_ITEM"; then
    export TF_VAR_hosted_data_snapshots="{}"
    log "hosted game servers: off (no '$HOSTED_ITEM' item in vault '$VAULT')"
    return
  fi
  map="$(hosted_snapshot_map)" \
    || die "Refusing to run terraform with hosting unknown (fix the vault item '$HOSTED_ITEM', or delete it to turn $DEP's hosting off)"
  export TF_VAR_hosted_data_snapshots="$map"
  log "hosted game servers: data sets $(jq -r 'to_entries | map("\(.key) (\(.value))") | join(", ")' <<<"$map") (vault item '$HOSTED_ITEM')"
}

# terraform in infra/aws, with the deployment's own data directory: each
# deployment's backend (its account's state bucket) stays its own.
tf() { TF_DATA_DIR="$TF_DIR/.terraform-$DEP" terraform -chdir="$TF_DIR" "$@"; }

# The deployment's stack, ready: the account's credentials (aws_auth, which
# terraform inherits), the deployment and its account from the vault
# (TF_VAR_deployment, TF_VAR_aws_account_id, which the AWS provider allows
# alone, TF_VAR_region, and prod's TF_VAR_service_eip_allocation_id: the
# gate's address in the prod account), its state (the S3 bucket the account's
# org stack made, with S3's own lock file) and its var-file. The var-files
# Terraform loads on its own (terraform.tfvars[.json], *.auto.tfvars[.json])
# are refused: they would reach every deployment.
TF_VAR_ARGS=()
tf_init() {
  local f stray=()
  for f in "$TF_DIR"/terraform.tfvars "$TF_DIR"/terraform.tfvars.json "$TF_DIR"/*.auto.tfvars*; do
    [ ! -e "$f" ] || stray+=("infra/aws/${f##*/}")
  done
  [ "${#stray[@]}" -eq 0 ] \
    || die "${stray[*]}: Terraform loads such a file into EVERY deployment's run. Move its settings into infra/aws/<deployment>.tfvars (DEPLOY.md, \"The var-file\")"
  [ -f "$TFVARS" ] || die "no infra/aws/$DEP.tfvars (deployment.tfvars.example names the knobs)"
  aws_auth
  [ -n "$STATE_BUCKET" ] \
    || die "$AWS_ITEM names no state_bucket: the org stack writes it (./deploy/run.sh org apply)"
  export TF_VAR_deployment="$DEP" TF_VAR_aws_account_id="$ACCOUNT_ID" TF_VAR_region="$REGION"
  if [ "$DEP" = prod ] && [ -n "$GATE_EIP_ALLOCATION_ID" ]; then
    export TF_VAR_service_eip_allocation_id="$GATE_EIP_ALLOCATION_ID"
  fi
  tf init -input=false -reconfigure \
    -backend-config="bucket=$STATE_BUCKET" -backend-config="key=infra/aws/terraform.tfstate" \
    -backend-config="region=$REGION" -backend-config="use_lockfile=true" -backend-config="encrypt=true" >/dev/null \
    || die "terraform init failed (the state bucket $STATE_BUCKET is the org stack's: ./deploy/run.sh org apply)"
  TF_VAR_ARGS=(-var-file="$TFVARS")
  log "$DEP: state s3://$STATE_BUCKET/infra/aws/terraform.tfstate, var-file infra/aws/$DEP.tfvars"
}

# terraform with the TF_VAR_* secrets of deploy/env/terraform.env.tpl in its
# environment alone, from the run's vault cache (the AWS credentials are
# aws_auth's, inherited).
tf_op() {
  TF_DATA_DIR="$TF_DIR/.terraform-$DEP" vault_env_run "$TF_ENV_TPL" terraform -chdir="$TF_DIR" "$@"
}

# The types whose destruction (or replacement) loses what no apply makes again,
# or takes the service down: an Elastic IP (the gate's retail clients use, the
# load balancer's other) and an association, an instance and a network
# interface, the load balancer (it holds the gate's address), the cluster and a
# program's service, the controller's Cloud Map namespace, the database, the
# cache (every web session goes with it), the operations bucket.
PRECIOUS_TYPES='["aws_eip","aws_eip_association","aws_network_interface","aws_instance","aws_lb","aws_ecs_cluster","aws_ecs_service","aws_service_discovery_private_dns_namespace","aws_db_instance","aws_elasticache_replication_group","aws_s3_bucket"]'

# infra apply: plan to a file, and refuse a plan that destroys (or replaces) a
# precious resource unless --allow-destroy, and any plan that makes prod a gate
# address (prod adopts the address retail clients use; service.tf's
# preconditions stop such a plan first). On prod, show what the plan does and
# apply that saved plan only after --yes or a typed prod; on staging apply it.
# The plan applied is the one shown, never a new one.
infra_apply() {
  local allow_destroy=0 yes=0 args=() plan="$SECRETS_DIR/tfplan" plan_json doomed made summary rc=0 a
  for a in "$@"; do
    case "$a" in
      --allow-destroy) allow_destroy=1 ;;
      --yes) yes=1 ;;
      *) args+=("$a") ;;
    esac
  done
  tf_op plan -input=false -out="$plan" "${TF_VAR_ARGS[@]}" ${args[@]+"${args[@]}"} \
    || die "the plan failed; nothing was applied"
  # Held in memory: the plan's JSON carries the stack's sensitive values.
  plan_json="$(tf show -json "$plan")" || { rm -f "$plan"; die "reading the plan failed; nothing was applied"; }
  doomed="$(jq -r --argjson types "$PRECIOUS_TYPES" \
    '[.resource_changes[]? | select((.change.actions | index("delete")) and (.type as $t | $types | index($t))) | .address] | join(" ")' <<<"$plan_json")" \
    || { rm -f "$plan"; die "reading the plan failed; nothing was applied"; }
  made="$(jq -r '[.resource_changes[]? | select((.address | startswith("aws_eip.gate")) and (.change.actions | index("create"))) | .address] | join(" ")' <<<"$plan_json")" \
    || { rm -f "$plan"; die "reading the plan failed; nothing was applied"; }
  summary="$(jq -r '[.resource_changes[]? | .change.actions
      | if index("delete") and index("create") then "replace"
        elif . == ["create"] then "create" elif . == ["update"] then "update"
        elif . == ["delete"] then "destroy" elif . == ["forget"] then "forget" else empty end]
    | group_by(.) | map("\(length) to \(.[0])") | join(", ")' <<<"$plan_json")" \
    || { rm -f "$plan"; die "reading the plan failed; nothing was applied"; }
  plan_json=""
  if [ "$DEP" = prod ] && [ -n "$made" ]; then
    rm -f "$plan"
    die "the plan makes prod a new gate address ($made). Refusing whatever the flags: prod adopts the address retail clients use (aws-prod/gate_eip_allocation_id; DEPLOY.md, \"The var-file\")"
  fi
  if [ -n "$doomed" ] && [ "$allow_destroy" -eq 0 ]; then
    rm -f "$plan"
    die "the plan destroys or replaces $doomed. Refusing: check the plan above, then infra apply --allow-destroy if that is meant"
  fi
  log "the plan for $DEP: ${summary:-no resource changes}${doomed:+; it destroys or replaces $doomed (--allow-destroy)}"
  # A refusal exits; the EXIT trap removes the saved plan.
  [ -z "$summary" ] || confirm_prod "apply this saved plan as it stands (nothing is planned again)" "$yes"
  tf_op apply -input=false "$plan" || rc=$?
  rm -f "$plan"
  [ "$rc" -eq 0 ] || exit "$rc"
}

cmd_infra() {
  local action="${1:-plan}"
  shift || true
  case "$action" in
    plan|apply|destroy|state-rm|import|output|list) ;;
    *) die "unknown: infra $action (try: plan|apply|destroy|state-rm|import|output|list)" ;;
  esac
  require_op
  tf_init
  case "$action" in
    plan)
      hosted_snapshot_var
      tf_op plan -input=false "${TF_VAR_ARGS[@]}" "$@" ;;
    apply)
      hosted_snapshot_var
      infra_apply "$@" ;;
    destroy)
      [ "$DEP" != prod ] || die "infra destroy refuses prod"
      take_yes "$@"
      confirm_word "$DEP" "destroy $DEP's whole stack (account $ACCOUNT_ID; the database keeps its final snapshot)" "$OPT_YES"
      hosted_snapshot_var
      tf_op destroy -input=false -auto-approve "${TF_VAR_ARGS[@]}" ${OPT_REST[@]+"${OPT_REST[@]}"} ;;
    state-rm)
      take_yes "$@"
      [ "${#OPT_REST[@]}" -ge 1 ] || die "infra state-rm needs an ADDRESS"
      confirm_prod "forget ${OPT_REST[*]} (their objects stay)" "$OPT_YES"
      tf state rm "${OPT_REST[@]}" ;;
    import)
      take_yes "$@"
      [ "${#OPT_REST[@]}" -eq 2 ] || die "infra import needs ADDRESS ID"
      confirm_prod "adopt ${OPT_REST[1]} as ${OPT_REST[0]}" "$OPT_YES"
      hosted_snapshot_var
      tf_op import -input=false "${TF_VAR_ARGS[@]}" "${OPT_REST[0]}" "${OPT_REST[1]}" ;;
    output) tf output "$@" ;;
    list)   tf state list "$@" ;;
  esac
}

# ---------------------------------------------------------------------------
# The other stacks: the Organization's (infra/org), and the two every
# deployment shares, the Cloudflare zone's (infra/zone) and the GitHub
# organization's (infra/github). All three keep their states in prod's state
# bucket (prod owns what is shared; the management account keeps none)
# ---------------------------------------------------------------------------

# Set by each command below: what the stack is, its directory and data
# directory, its state's key in the state bucket, the env template its
# secrets come from (none: AWS credentials alone) and the word its apply asks
# for.
STACK_LABEL=""
STACK_DIR=""
STACK_DATA_DIR=""
STACK_KEY=""
STACK_ENV_TPL=""
STACK_WORD=""

stack_tf() { TF_DATA_DIR="$STACK_DATA_DIR" terraform -chdir="$STACK_DIR" "$@"; }
stack_tf_op() {
  if [ -z "$STACK_ENV_TPL" ]; then
    stack_tf "$@"
    return
  fi
  TF_DATA_DIR="$STACK_DATA_DIR" vault_env_run "$STACK_ENV_TPL" terraform -chdir="$STACK_DIR" "$@"
}

# plan|apply|import|output|list of the stack the STACK_* name, its state in
# the S3 bucket $STATE_BUCKET (S3's own lock file holds a run's lock); apply
# plans to a file and applies that plan after --yes or the typed STACK_WORD.
stack_run() {
  [ -n "$STATE_BUCKET" ] || die "no state bucket for $STACK_LABEL (the org stack writes it to the vault)"
  stack_tf init -input=false -reconfigure \
    -backend-config="bucket=$STATE_BUCKET" -backend-config="key=$STACK_KEY" \
    -backend-config="region=$REGION" -backend-config="use_lockfile=true" -backend-config="encrypt=true" >/dev/null \
    || die "terraform init of $STACK_DIR failed (state s3://$STATE_BUCKET/$STACK_KEY)"
  log "$STACK_LABEL: state s3://$STATE_BUCKET/$STACK_KEY"
  stack_run_body "$@"
}

# stack_run's command, once terraform is initialized.
stack_run_body() {
  local action="${1:-plan}" rc=0 plan="$SECRETS_DIR/tfplan"
  shift || true
  case "$action" in
    plan|apply|import|output|list) ;;
    *) die "unknown: $STACK_LABEL $action (try: plan|apply|import|output|list)" ;;
  esac
  case "$action" in
    plan) stack_tf_op plan -input=false "$@" ;;
    apply)
      take_yes "$@"
      stack_tf_op plan -input=false -out="$plan" ${OPT_REST[@]+"${OPT_REST[@]}"} \
        || die "the plan failed; nothing was applied"
      # A refusal exits; the EXIT trap removes the saved plan.
      confirm_word "$STACK_WORD" "apply this saved plan to $STACK_LABEL" "$OPT_YES"
      stack_tf_op apply -input=false "$plan" || rc=$?
      rm -f "$plan"
      [ "$rc" -eq 0 ] || exit "$rc" ;;
    import)
      [ $# -eq 2 ] || die "$STACK_LABEL import needs ADDRESS ID"
      stack_tf_op import -input=false "$1" "$2" ;;
    output) stack_tf output "$@" ;;
    list)   stack_tf state list "$@" ;;
  esac
}

# The Organization's own stack (infra/org), as the management account's
# opennova-org user (vault item aws-org): the Organization and both member
# accounts, and in each account (through its OrganizationAccountAccessRole)
# what everything else stands on: its state bucket, the role boundary, the
# deploy user and its access key, the GitHub deploy role. Its own state lives
# in prod's state bucket, which its first run makes: that run applies with a
# local state (a backend override written and removed here), then moves the
# state there. Each apply writes what it made back to the vault
# (aws-<deployment>: state_bucket, access_key_id, secret_access_key,
# github_deploy_role_arn), where every later command reads it: on the first
# run only once the state has moved, so a move that fails leaves aws-prod
# naming no state_bucket and the next run picks up the local state again.
ORG_OVERRIDE="$ORG_TF_DIR/backend_override.tf"
cmd_org() {
  local action="${1:-plan}"
  STACK_LABEL="the Organization (the management account and both deployments' accounts)"
  STACK_DIR="$ORG_TF_DIR"
  STACK_DATA_DIR="$ORG_TF_DIR/.terraform"
  STACK_KEY="org/terraform.tfstate"
  STACK_ENV_TPL="$ORG_ENV_TPL"
  STACK_WORD=org
  require_op
  org_auth
  local prod_json prod_account prod_bucket
  prod_json="$(item_json aws-prod)"
  prod_account="$(json_field "$prod_json" account_id)"
  prod_bucket="$(json_field "$prod_json" state_bucket)"
  [[ "$prod_account" =~ ^[0-9]{12}$ ]] || die "aws-prod names no account_id (DEPLOY.md, \"The Organization and the accounts\")"
  if [ -n "$prod_bucket" ]; then
    # An interrupted first run can leave its local-backend override behind.
    rm -f "$ORG_OVERRIDE"
    org_backend_init "$prod_bucket" "$prod_account" -reconfigure \
      || die "terraform init of $STACK_DIR failed (state s3://$prod_bucket/$STACK_KEY)"
    log "$STACK_LABEL: state s3://$prod_bucket/$STACK_KEY"
    stack_run_body "$@"
  else
    # The first run: no bucket holds the state yet.
    case "$action" in
      plan|apply) ;;
      *) die "the org stack has no state yet (aws-prod names no state_bucket): ./deploy/run.sh org apply makes it" ;;
    esac
    log "the org stack's first run: a local state, moved into prod's state bucket once applied"
    printf '%s\n' '# Written by the deploy toolbox for the org stack'"'"'s first run alone (deploy/bin/lib/infra.sh).' \
      'terraform {' '  backend "local" {}' '}' > "$ORG_OVERRIDE"
    local rc=0
    ( stack_tf init -input=false -reconfigure >/dev/null || die "terraform init of $STACK_DIR failed"
      stack_run_body "$@" ) || rc=$?
    if [ "$rc" -ne 0 ] || [ "$action" != apply ]; then
      rm -f "$ORG_OVERRIDE"
      [ "$rc" -eq 0 ] || exit "$rc"
      return 0
    fi
  fi
  [ "$action" = apply ] || return 0
  if [ -f "$ORG_OVERRIDE" ]; then
    # The first run's state moves before the vault learns of the bucket: a
    # move that fails leaves the local state and a vault that still sends the
    # next run here. Once it has moved, prod's state_bucket is written first
    # and alone, so a later write that fails still sends the next run to S3
    # (whose apply writes everything back again); the local files go only then.
    prod_bucket="$(stack_tf output -json deployments | jq -r '.prod.state_bucket // empty')"
    rm -f "$ORG_OVERRIDE"
    [ -n "$prod_bucket" ] || die "the org stack names no prod state_bucket: its local state is $ORG_TF_DIR/terraform.tfstate (keep it; run org apply again)"
    org_backend_init "$prod_bucket" "$prod_account" -migrate-state -force-copy \
      || die "moving the org stack's state into s3://$prod_bucket/$STACK_KEY failed: its local state is $ORG_TF_DIR/terraform.tfstate (keep it; run org apply again)"
    log "the org stack's state: s3://$prod_bucket/$STACK_KEY"
    # shellcheck disable=SC2034  # vault_set_fields' (stripe.sh) values
    VAULT_VALUES["state_bucket"]="$prod_bucket"
    ( vault_set_fields aws-prod state_bucket:STRING ) \
      || die "the org stack's state is in s3://$prod_bucket/$STACK_KEY, but writing it to the vault failed: set aws-prod's field state_bucket to $prod_bucket, then run org apply again"
    rm -f "$ORG_TF_DIR/terraform.tfstate" "$ORG_TF_DIR/terraform.tfstate.backup"
  fi
  org_write_back
}

# terraform init of the org stack on its S3 backend: prod's state bucket ($1),
# reached through prod's OrganizationAccountAccessRole (account $2), the
# settings in a file of the tmpfs; the init's mode is the caller's ($3..):
# -reconfigure on a run, -migrate-state -force-copy for the first run's move
# (Terraform takes the two modes only apart). Fails as terraform does.
org_backend_init() {
  local bucket="$1" account="$2" file="$SECRETS_DIR/org.backend.hcl" rc=0
  shift 2
  [ $# -gt 0 ] || die "org_backend_init: no init mode (-reconfigure, or -migrate-state -force-copy)"
  (umask 077 && printf '%s\n' "bucket       = \"$bucket\"" "key          = \"$STACK_KEY\"" \
    "region       = \"$REGION\"" "use_lockfile = true" "encrypt      = true" \
    "assume_role = {" "  role_arn = \"arn:aws:iam::$account:role/OrganizationAccountAccessRole\"" "}" > "$file")
  stack_tf init -input=false "$@" -backend-config="$file" >/dev/null || rc=$?
  rm -f "$file"
  return "$rc"
}

# What the org stack made, into the vault: each deployment's state bucket,
# its deploy user's access key and its GitHub deploy role (aws-<deployment>),
# and the Identity Center access portal's address (aws-org).
# The key goes vault-ward through a template file in the tmpfs, never an
# argument, never printed.
org_write_back() {
  local outputs keys dep field value wrote
  outputs="$(stack_tf output -json deployments)" || die "the org stack names no deployments output"
  keys="$(stack_tf output -json deploy_keys)" || die "the org stack names no deploy_keys output"
  # Each deployment's state_bucket goes last, prod's last of all: it is what
  # sends a later run (this stack's own, and every deployment command) to S3.
  for dep in staging prod; do
    wrote=()
    for field in github_deploy_role_arn state_bucket; do
      value="$(jq -r --arg d "$dep" --arg f "$field" '.[$d][$f] // empty' <<<"$outputs")"
      [ -n "$value" ] || die "the org stack's deployments output names no $field for $dep"
      # shellcheck disable=SC2034  # vault_set_fields' (stripe.sh) values
      VAULT_VALUES["$field"]="$value"
      wrote+=("$field:STRING")
    done
    VAULT_VALUES["access_key_id"]="$(jq -r --arg d "$dep" '.[$d].access_key_id // empty' <<<"$keys")"
    VAULT_VALUES["secret_access_key"]="$(jq -r --arg d "$dep" '.[$d].secret_access_key // empty' <<<"$keys")"
    if [ -z "${VAULT_VALUES[access_key_id]}" ] || [ -z "${VAULT_VALUES[secret_access_key]}" ]; then
      die "the org stack's deploy_keys output names no key for $dep"
    fi
    wrote+=(access_key_id:STRING secret_access_key:CONCEALED)
    vault_set_fields "aws-$dep" "${wrote[@]}"
    VAULT_VALUES["secret_access_key"]=""
    log "vault aws-$dep: ${wrote[*]%%:*}"
  done
  keys=""
  value="$(stack_tf output -raw sso_portal_url)" || die "the org stack names no sso_portal_url output"
  VAULT_VALUES["sso_portal_url"]="$value"
  vault_set_fields aws-org sso_portal_url:STRING
  log "vault aws-org: sso_portal_url $value"
}

# A shared stack (the zone's, the organization's), its state in prod's state
# bucket, reached as prod's deploy user.
shared_stack() {
  require_op
  set_deployment prod
  aws_auth
  [ -n "$STATE_BUCKET" ] || die "aws-prod names no state_bucket: the org stack makes it (./deploy/run.sh org apply)"
  stack_run "$@"
}

# The Cloudflare zone's shared settings: every deployment's site lives in it.
cmd_zone() {
  STACK_LABEL="the Cloudflare zone (every deployment's site)"
  STACK_DIR="$ZONE_TF_DIR"
  STACK_DATA_DIR="$ZONE_TF_DIR/.terraform"
  STACK_KEY="infra/zone/terraform.tfstate"
  STACK_ENV_TPL="$ZONE_ENV_TPL"
  STACK_WORD=zone
  shared_stack "$@"
}

# The GitHub organization: one stack, no deployment.
cmd_github() {
  STACK_LABEL="the GitHub organization"
  STACK_DIR="$GH_TF_DIR"
  STACK_DATA_DIR="$GH_TF_DIR/.terraform"
  STACK_KEY="infra/github/terraform.tfstate"
  STACK_ENV_TPL="$GH_ENV_TPL"
  STACK_WORD=github
  shared_stack "$@"
}
