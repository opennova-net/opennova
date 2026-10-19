# shellcheck shell=bash
# on-deploy's secrets: the vault's items for the deployment (made, checked)
# and its runtime secrets copied to SSM (`secrets init|check|sync`).
# deploy/bin/on-deploy sources this file before it runs a command: it defines
# functions and their constants alone, and runs nothing itself. The helpers
# every command shares (log, die, the deployment, the vault, AWS) are
# on-deploy's own.

# ---------------------------------------------------------------------------
# secrets
# ---------------------------------------------------------------------------

STRIPE_FIELDS=(secret_key webhook_secret price_tier1 price_tier2 price_tier3 price_own_machine price_static_address)

# Whether a Stripe item's field value has its form ($3: the item's mode).
stripe_field_ok() {
  local field="$1" value="$2" mode="$3"
  case "$field" in
    secret_key)     [[ "$value" =~ ^(sk|rk)_${mode}_[A-Za-z0-9]{10,}$ ]] ;;
    webhook_secret) [[ "$value" =~ ^whsec_[A-Za-z0-9+/=]{16,}$ ]] ;;
    price_*)        [[ "$value" =~ ^price_[A-Za-z0-9]{8,}(,price_[A-Za-z0-9]{8,})*$ ]] ;;
    *) return 1 ;;
  esac
}

# A Stripe item's state, without printing a value: STRIPE_STATE ok,
# placeholders, absent or failed, and STRIPE_DETAIL saying which fields.
stripe_check_item() {
  local item="$1" mode="$2" json field value
  local placeholders=() failed=()
  STRIPE_STATE="" STRIPE_DETAIL=""
  if ! vault_has_item "$item"; then
    STRIPE_STATE=absent
    STRIPE_DETAIL="absent (billing off)"
    return 0
  fi
  json="$(item_json "$item")"
  for field in "${STRIPE_FIELDS[@]}"; do
    if ! json_has_field "$json" "$field"; then
      placeholders+=("$field")
      continue
    fi
    value="$(jq -r --arg l "$field" '[.fields[]? | select(.label == $l) | (.value // "")][0]' "$json")"
    if [ -z "$value" ] || [ "$value" = REPLACE_ME ]; then
      placeholders+=("$field")
    elif ! stripe_field_ok "$field" "$value" "$mode"; then
      if [ "$field" = secret_key ] && [[ "$value" =~ ^(sk|rk)_(test|live)_ ]]; then
        failed+=("secret_key (a ${BASH_REMATCH[2]} key in a $mode item)")
      else
        failed+=("$field (malformed)")
      fi
    fi
  done
  value=""
  if [ "${#failed[@]}" -gt 0 ]; then
    STRIPE_STATE=failed
    STRIPE_DETAIL="FAILED: $(IFS=,; printf '%s' "${failed[*]}" | sed 's/,/, /g')"
  elif [ "${#placeholders[@]}" -gt 0 ]; then
    STRIPE_STATE=placeholders
    STRIPE_DETAIL="placeholders: $(IFS=,; printf '%s' "${placeholders[*]}" | sed 's/,/, /g') (billing off)"
  else
    STRIPE_STATE=ok
    STRIPE_DETAIL="OK ($mode mode, 3 tiers + dedicated machine + static address)"
  fi
}

# Add the fields item $1 lacks (category $2 when it is new), each spec
# label:kind:arg:type: kind `gen` makes `openssl rand -hex <arg>`, `value`
# writes <arg>; type CONCEALED or STRING. Never overwrites a field; prints no
# value (the values reach op through a template file in the tmpfs, never an
# argument). The item is read fresh (the vault's own, not the run's cache).
ensure_item() {
  local title="$1" category="$2" json new spec label kind arg type creating=0 generated
  shift 2
  local added=() kept=()
  json="$SECRETS_DIR/item.ensure.json"
  new="$SECRETS_DIR/item.ensure-new.json"
  if vault_has_item "$title"; then
    vault_item_load "$title" fresh || die "reading vault item '$title' failed: $VAULT_ERR"
    (umask 077 && cp "$(vault_item_file "$title")" "$json")
  else
    creating=1
    (umask 077 && jq -n --arg t "$title" --arg c "$category" '{title: $t, category: $c, fields: []}' > "$json")
  fi
  chmod 600 "$json"
  for spec in "$@"; do
    IFS=: read -r label kind arg type <<<"$spec"
    if json_has_field "$json" "$label"; then
      kept+=("$label")
      continue
    fi
    case "$kind" in
      gen)   generated="$(openssl rand -hex "$arg")" ;;
      value) generated="$arg" ;;
      *) die "ensure_item: unknown kind $kind" ;;
    esac
    OPENNOVA_FIELD_VALUE="$generated" jq --arg l "$label" --arg t "$type" \
      '.fields += [{label: $l, type: $t, value: env.OPENNOVA_FIELD_VALUE}]' "$json" > "$new"
    mv -f "$new" "$json"
    added+=("$label")
  done
  generated=""
  if [ "${#added[@]}" -eq 0 ]; then
    log "  $title: has ${kept[*]}"
  elif [ "$creating" -eq 1 ]; then
    vault_item_create "$json" || die "creating vault item '$title' failed"
    log "  $title: created with ${added[*]}"
  else
    vault_item_edit "$title" "$json" || die "adding ${added[*]} to vault item '$title' failed"
    log "  $title: added ${added[*]}${kept[*]:+ (kept ${kept[*]})}"
  fi
  rm -f "$json" "$new"
}

stripe_placeholder_specs() {
  printf '%s\n' secret_key:value:REPLACE_ME:CONCEALED webhook_secret:value:REPLACE_ME:CONCEALED \
    price_tier1:value:REPLACE_ME:STRING price_tier2:value:REPLACE_ME:STRING \
    price_tier3:value:REPLACE_ME:STRING price_own_machine:value:REPLACE_ME:STRING \
    price_static_address:value:REPLACE_ME:STRING
}

secrets_init() {
  require_op
  log "vault '$VAULT', deployment $DEP: adding what is missing (no value is printed or overwritten)…"
  # The deployment's own: its app item (the edge authentication secret among
  # it: each deployment's web hostnames carry their own, infra/zone) and its
  # database's master password.
  ensure_item "$APP_ITEM" SERVER admin_api_token:gen:24:CONCEALED database_password:gen:24:CONCEALED \
    redis_password:gen:32:CONCEALED edge_auth_secret:gen:32:CONCEALED
  ensure_item "$RDS_ITEM" SECURE_NOTE \
    master_password:gen:24:CONCEALED
  # The deployment's AWS account (an account of its own in the Organization):
  # its id and its root user's address (filled in by hand once the account is
  # made), its name and region; `org apply` adds state_bucket, the deploy
  # user's access_key_id and secret_access_key and github_deploy_role_arn;
  # prod's gate_eip_allocation_id is filled in by hand (the gate's Elastic IP in
  # the prod account, DEPLOY.md "The var-file").
  ensure_item "$AWS_ITEM" SECURE_NOTE \
    account_id:value:REPLACE_ME:STRING account_name:value:"opennova-$DEP":STRING \
    region:value:us-east-1:STRING root_email:value:REPLACE_ME:STRING
  # The management account's: its opennova-org user's access key (made by
  # hand, DEPLOY.md "The Organization and the accounts"), the account's id,
  # the Organization's, the region, and the maintainer's Identity Center user
  # (its names and email); `org apply` adds sso_portal_url. The org stack runs
  # with it alone.
  ensure_item aws-org API_CREDENTIAL \
    account_id:value:REPLACE_ME:STRING organization_id:value:REPLACE_ME:STRING \
    region:value:us-east-1:STRING sso_given_name:value:REPLACE_ME:STRING \
    sso_family_name:value:REPLACE_ME:STRING sso_email:value:REPLACE_ME:STRING
  # The CI runners' account (infra/ci): its id and its root user's address
  # (filled in by hand once the account is made), its name and region. It has
  # no deploy user and no state bucket: the ci stack runs as opennova-org.
  ensure_item aws-ci SECURE_NOTE \
    account_id:value:REPLACE_ME:STRING account_name:value:opennova-ci:STRING \
    region:value:us-east-1:STRING root_email:value:REPLACE_ME:STRING
  # Shared on purpose: where the alarms write, where support@<domain> forwards
  # (infra/zone; REPLACE_ME keeps email routing off), and Stripe's two modes.
  ensure_item alerts SECURE_NOTE \
    email:value:REPLACE_ME:STRING
  ensure_item email SECURE_NOTE \
    forward_to:value:REPLACE_ME:STRING
  local specs=()
  mapfile -t specs < <(stripe_placeholder_specs)
  # stripe-test's restricted_key: a restricted key with the server's
  # permissions alone, for the local stack (deploy/env/billing.dev.env.tpl)
  # and CI (the Actions secret STRIPE_TEST_SECRET_KEY, infra/github;
  # REPLACE_ME: the billing end-to-end test skips). Neither ever holds the
  # mode's secret_key, which is staging's own.
  ensure_item stripe-test API_CREDENTIAL "${specs[@]}" restricted_key:value:REPLACE_ME:CONCEALED
  ensure_item stripe-live API_CREDENTIAL "${specs[@]}"
  log "done. In the 1Password app fill alerts/email, email/forward_to, each Stripe item's secret_key and"
  log "stripe-test's restricted_key; ./deploy/run.sh --deployment staging stripe setup (test mode) and"
  log "--deployment prod stripe setup (live mode) fill the rest of the Stripe fields (DEPLOY.md,"
  log "\"Monthly support tiers (Stripe)\"). Then ./deploy/run.sh$DEP_FLAG secrets check and secrets sync."
}

# A runtime.env.tpl line: NAME=op://... [# optional|# billing]. Sets RT_NAME,
# RT_REF, RT_FLAG; fails on a line that is none (comments, blanks).
RT_RE='^([A-Z][A-Z0-9_]*)=(op://[^[:space:]#]+)[[:space:]]*(#[[:space:]]*(optional|billing))?[[:space:]]*$'
parse_runtime_line() {
  [[ "$1" =~ $RT_RE ]] || return 1
  RT_NAME="${BASH_REMATCH[1]}"
  RT_REF="${BASH_REMATCH[2]}"
  RT_FLAG="${BASH_REMATCH[4]}"
}

# Whether reference op://vault/item/field names a field the item has.
ref_field_exists() {
  local path="${1#op://}" item field json
  item="$(cut -d/ -f2 <<<"$path")"
  field="$(cut -d/ -f3 <<<"$path")"
  vault_has_item "$item" || return 1
  json="$(item_json "$item")"
  json_has_field "$json" "$field"
}

secrets_check() {
  require_op
  log "checking vault '$VAULT' for deployment $DEP (no value is printed)…"
  local rc=0 tpl line
  for tpl in "$TF_ENV_TPL" "$AWS_ENV_TPL"; do
    if vault_tpl_resolve "$tpl" check; then
      log "  $(basename "$tpl")    OK"
    else
      err "  $(basename "$tpl")    FAILED: $VAULT_ERR"; rc=1
    fi
  done
  # The zone's (infra/zone): every deployment's edge secret, so it resolves
  # once each deployment's secrets init ran.
  if vault_tpl_resolve "$ZONE_ENV_TPL" check; then
    log "  $(basename "$ZONE_ENV_TPL")    OK"
  else
    log "  $(basename "$ZONE_ENV_TPL")    not yet (secrets init for each deployment makes its app item's edge_auth_secret; zone plan needs both)"
  fi
  # The runtime secrets: each required reference must resolve.
  while IFS= read -r line || [ -n "$line" ]; do
    parse_runtime_line "$line" || continue
    case "$RT_FLAG" in
      billing) continue ;;
      optional)
        if ref_field_exists "$RT_REF"; then log "  $RT_NAME    OK (optional)"; else log "  $RT_NAME    absent (optional)"; fi
        continue ;;
    esac
    if vault_ref "$RT_REF"; then
      log "  $RT_NAME    OK"
    else
      err "  $RT_NAME    FAILED ($RT_REF)"; rc=1
    fi
    VAULT_VALUE=""
  done < <(tpl_text "$RUNTIME_TPL")
  # The Stripe items: test mode's and live mode's.
  local item mode
  for item in stripe-test stripe-live; do
    mode="test"
    [ "$item" != stripe-live ] || mode="live"
    stripe_check_item "$item" "$mode"
    if [ "$STRIPE_STATE" = failed ]; then
      err "  $item    $STRIPE_DETAIL"; rc=1
    else
      log "  $item    $STRIPE_DETAIL"
    fi
  done
  log "  (deployment $DEP syncs $STRIPE_ITEM)"
  # The test mode's restricted key (the local stack's and CI's): never a
  # secret key, never a live one.
  local restricted=""
  if vault_has_item stripe-test && vault_value stripe-test restricted_key; then
    restricted="$VAULT_VALUE"
    VAULT_VALUE=""
    if [ -z "$restricted" ] || [ "$restricted" = REPLACE_ME ]; then
      log "  stripe-test/restricted_key    placeholder (the local stack's billing and CI's billing test off)"
    elif [[ "$restricted" =~ ^rk_test_[A-Za-z0-9]{10,}$ ]]; then
      log "  stripe-test/restricted_key    OK (rk_test_ form)"
    else
      err "  stripe-test/restricted_key    FAILED: not a test-mode restricted key (rk_test_...)"; rc=1
    fi
  else
    log "  stripe-test/restricted_key    absent (secrets init makes it)"
  fi
  restricted=""
  # Where the alarms and budgets write: a placeholder sends nothing.
  if vault_value alerts email && [[ "$VAULT_VALUE" == *@*.* ]]; then
    log "  alerts/email    OK"
  else
    log "  alerts/email    placeholder or absent: the alarms, budgets and cost anomalies email nobody"
  fi
  # Where the domain's email routes forward (the zone's, infra/zone), and the
  # SES sandbox's recipient (each deployment's account verifies it).
  # terraform.env.tpl names the field, so an absent item fails its line above.
  if ! vault_has_item email; then
    err "  email/forward_to    absent: every infra run needs it (secrets init makes it, REPLACE_ME: email routing off)"; rc=1
  elif vault_value email forward_to && [[ "$VAULT_VALUE" == *@*.* ]]; then
    log "  email/forward_to    OK: support@<domain> forwards there (once Cloudflare's verification link is clicked)"
  else
    log "  email/forward_to    placeholder: email routing off (support@<domain> receives nothing)"
  fi
  VAULT_VALUE=""
  [ "$rc" -eq 0 ] || die "one or more checks failed: create or fix the vault items (DEPLOY.md, section 1; secrets init adds the generated ones)"
  log "all references resolved."
}

# Copy the deployment's runtime secrets (runtime.env.tpl) from the vault to
# SSM SecureStrings, /opennova/<deployment>/secret/<NAME>, which ECS hands the
# tasks whose task definitions name them. Each value goes
# vault (the run's cache) -> a 0600 file in the tmpfs -> `put-parameter
# --value file://...`: never an argument, never printed.
secrets_sync() {
  local prune=0 yes=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --prune) prune=1; shift ;;
      --yes) yes=1; shift ;;
      *) die "unknown: secrets sync $1 (try: --prune, --yes)" ;;
    esac
  done
  require_op
  confirm_prod "overwrite prod's runtime secrets in SSM with the vault's" "$yes"
  aws_auth
  local billing=0
  stripe_check_item "$STRIPE_ITEM" "$STRIPE_MODE"
  case "$STRIPE_STATE" in
    ok) billing=1; log "billing: $STRIPE_ITEM $STRIPE_DETAIL" ;;
    failed) die "$STRIPE_ITEM: $STRIPE_DETAIL. Fix it (secrets check), or delete the item to sync with billing off" ;;
    *) log "billing: off ($STRIPE_ITEM $STRIPE_DETAIL); its parameters are not synced" ;;
  esac
  log "syncing vault '$VAULT' -> $SSM_PREFIX/secret/ (deployment $DEP)…"
  local line value_file="$SECRETS_DIR/value.sync" synced=()
  while IFS= read -r line || [ -n "$line" ]; do
    parse_runtime_line "$line" || continue
    if [ "$RT_FLAG" = billing ] && [ "$billing" -eq 0 ]; then continue; fi
    if [ "$RT_FLAG" = optional ] && ! ref_field_exists "$RT_REF"; then continue; fi
    vault_ref "$RT_REF" || die "reading $RT_NAME ($RT_REF) failed: $VAULT_ERR; nothing more was synced"
    (umask 077 && printf '%s' "$VAULT_VALUE" > "$value_file")
    VAULT_VALUE=""
    chmod 600 "$value_file"
    if [ ! -s "$value_file" ]; then rm -f "$value_file"; die "$RT_NAME ($RT_REF) is empty"; fi
    aws ssm put-parameter --name "$SSM_PREFIX/secret/$RT_NAME" --type SecureString --tier Standard \
      --value "file://$value_file" --overwrite >/dev/null \
      || { rm -f "$value_file"; die "writing $SSM_PREFIX/secret/$RT_NAME failed"; }
    rm -f "$value_file"
    # The deployment's tag, as on everything the stack makes (put-parameter
    # takes no tags with --overwrite).
    aws ssm add-tags-to-resource --resource-type Parameter --resource-id "$SSM_PREFIX/secret/$RT_NAME" \
      --tags "Key=opennova:deployment,Value=opennova-$DEP" >/dev/null \
      || die "tagging $SSM_PREFIX/secret/$RT_NAME failed"
    synced+=("$RT_NAME")
    log "  $RT_NAME"
  done < <(tpl_text "$RUNTIME_TPL")
  # What SSM holds that the vault no longer names.
  local names name stale=()
  names="$(aws ssm get-parameters-by-path --path "$SSM_PREFIX/secret" --recursive \
             --query 'Parameters[].Name' --output text)" || die "listing $SSM_PREFIX/secret/ failed"
  for name in $names; do
    name="${name##*/}"
    [[ " ${synced[*]} " == *" $name "* ]] || stale+=("$name")
  done
  for name in ${stale[@]+"${stale[@]}"}; do
    if [ "$prune" -eq 1 ]; then
      aws ssm delete-parameter --name "$SSM_PREFIX/secret/$name" >/dev/null \
        || die "deleting $SSM_PREFIX/secret/$name failed"
      log "  $name deleted (the vault no longer names it)"
    else
      log "  $name kept, though the vault no longer names it (secrets sync --prune deletes it)"
    fi
  done
  log "synced ${#synced[@]} secrets. The tasks read them as they start: ./deploy/run.sh$DEP_FLAG app converge (or the next app deploy)."
}

cmd_secrets() {
  local action="${1:-check}"
  shift || true
  case "$action" in
    init)  secrets_init "$@" ;;
    check) secrets_check "$@" ;;
    sync)  secrets_sync "$@" ;;
    *) die "unknown: secrets $action (try: init|check|sync)" ;;
  esac
}
