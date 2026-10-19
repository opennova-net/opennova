# shellcheck shell=bash
# on-deploy's Stripe: the support tiers' objects in one Stripe mode, checked or
# made, their ids in the vault (`stripe check|setup`).
# deploy/bin/on-deploy sources this file before it runs a command: it defines
# functions and their constants alone, and runs nothing itself. The helpers
# every command shares (log, die, the deployment, the vault, AWS) are
# on-deploy's own.

# ---------------------------------------------------------------------------
# stripe: the support tiers' objects in the deployment's Stripe mode (DEPLOY.md,
# "Monthly support tiers (Stripe)"): live for prod (stripe-live), test for
# staging (stripe-test), each on its own site
# ---------------------------------------------------------------------------
#
# The deployment's Stripe item's secret_key reaches curl through a config file
# in the tmpfs (-K), never an argument; nothing prints a key or a signing
# secret, and the fields go back to the vault through a template file, as
# ensure_item writes them. Price ids, product ids and URLs are no secrets.

STRIPE_API=https://api.stripe.com
# The API version the server pins (apps/novaworld_common/billing): the webhook
# endpoint's events arrive in its shape.
STRIPE_API_VERSION=2025-03-31.basil
# The events the server's webhook handles.
STRIPE_EVENTS=(checkout.session.completed checkout.session.expired
  customer.subscription.created customer.subscription.updated customer.subscription.deleted
  customer.subscription.paused customer.subscription.resumed
  invoice.paid invoice.payment_failed invoice.payment_action_required customer.deleted)
# The catalog, one monthly USD price per entry: vault field|lookup key|product
# name|product description|metadata opennova_item|default amount (cents).
STRIPE_CATALOG=(
  "price_tier1|opennova_tier1_monthly|OpenNova support tier 1|Monthly support for OpenNova: 1 hosted game server|tier1|500"
  "price_tier2|opennova_tier2_monthly|OpenNova support tier 2|Monthly support for OpenNova: 2 hosted game servers|tier2|1000"
  "price_tier3|opennova_tier3_monthly|OpenNova support tier 3|Monthly support for OpenNova: 3 hosted game servers|tier3|1500"
  "price_own_machine|opennova_own_machine_monthly|OpenNova dedicated machine|One more hosted server on a machine of its own, no support tier needed|own_machine|4500"
  "price_static_address|opennova_static_address_monthly|OpenNova static address|A static IPv4 address and a DNS name for one dedicated machine, monthly add-on|static_address|500"
)
# The Customer Portal configuration this toolbox owns carries this metadata.
STRIPE_PORTAL_TAG=support_tiers

STRIPE_CFG=""
STRIPE_REPLY=""
STRIPE_HTTP=""
STRIPE_DOMAIN=""
STRIPE_WEB_HOST=""

# The mode's key from the vault into curl's config file; checks its form and
# mode first (a test key in stripe-live is refused), and finds the domain.
stripe_auth() {
  local mode="$1" key=""
  require_op
  vault_has_item "$STRIPE_ITEM" || die "vault item '$STRIPE_ITEM' is absent: ./deploy/run.sh$DEP_FLAG secrets init makes it"
  vault_value "$STRIPE_ITEM" secret_key || die "reading $STRIPE_ITEM/secret_key failed: $VAULT_ERR"
  key="$VAULT_VALUE"
  VAULT_VALUE=""
  if [ -z "$key" ] || [ "$key" = REPLACE_ME ]; then
    die "$STRIPE_ITEM/secret_key is a placeholder: put the $mode mode key there (Stripe, Developers, API keys)"
  fi
  if ! stripe_field_ok secret_key "$key" "$mode"; then
    if [[ "$key" =~ ^(sk|rk)_(test|live)_ ]]; then
      die "$STRIPE_ITEM/secret_key is a ${BASH_REMATCH[2]} mode key; $STRIPE_ITEM takes a $mode mode one"
    fi
    die "$STRIPE_ITEM/secret_key is malformed (sk_${mode}_... or rk_${mode}_...)"
  fi
  STRIPE_CFG="$SECRETS_DIR/value.stripe.curl"
  STRIPE_REPLY="$SECRETS_DIR/value.stripe-reply"
  (umask 077 && printf 'header = "Authorization: Bearer %s"\nheader = "Stripe-Version: %s"\n' \
    "$key" "$STRIPE_API_VERSION" > "$STRIPE_CFG")
  key=""
  vault_value cloudflare domain \
    || die "reading cloudflare/domain failed (the site's domain: the webhook's and the portal's URLs): $VAULT_ERR"
  STRIPE_DOMAIN="$VAULT_VALUE"
  VAULT_VALUE=""
  [[ "$STRIPE_DOMAIN" =~ ^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$ ]] \
    || die "cloudflare/domain '$STRIPE_DOMAIN' is no domain name"
  # The deployment's site, as infra/aws/locals.tf names it: prod's the domain,
  # staging's staging.<domain>. A mode's webhook and portal never name the
  # other deployment's.
  if [ "$DEP" = prod ]; then STRIPE_WEB_HOST="$STRIPE_DOMAIN"; else STRIPE_WEB_HOST="$DEP.$STRIPE_DOMAIN"; fi
}

# One Stripe API call: METHOD PATH, then curl's data arguments (GET: query
# parameters). The reply lands in STRIPE_REPLY (the tmpfs; a webhook's reply
# holds its signing secret), its status in STRIPE_HTTP. Only no answer at all
# stops the command; the caller reads the status.
stripe_call() {
  local method="$1" path="$2"
  shift 2
  local args=(-sS --max-time 30 -K "$STRIPE_CFG" -o "$STRIPE_REPLY" -w '%{http_code}')
  if [ "$method" = GET ]; then args+=(-G); else args+=(-X "$method"); fi
  STRIPE_HTTP="$(umask 077 && curl "${args[@]}" "$@" "$STRIPE_API$path")" \
    || die "Stripe did not answer $method $path"
}

# Stripe's error, from the last reply: its type, code and message.
stripe_error() {
  jq -r '[.error.type, .error.code, .error.param, .error.message] | map(select(. != null)) | join(" ")' \
    "$STRIPE_REPLY" 2>/dev/null | cut -c1-300
}

# The last call answered 200, or the command stops with Stripe's error ($1:
# what it was).
stripe_ok() {
  [ "$STRIPE_HTTP" = 200 ] || die "$1: Stripe answered $STRIPE_HTTP: $(stripe_error)"
}

# A dollar amount ($2, from flag $1: 5 or 5.00) in cents.
usd_cents() {
  [[ "$2" =~ ^([0-9]{1,5})(\.([0-9]{2}))?$ ]] || die "$1 takes US dollars (5, or 5.00), not '$2'"
  local cents=$((10#${BASH_REMATCH[1]} * 100 + 10#${BASH_REMATCH[3]:-0}))
  [ "$cents" -gt 0 ] || die "$1 must be more than 0"
  printf '%s' "$cents"
}

# The portal's settings, as curl data arguments, for the tier products $@
# (product:price each): switching among them prorates, a downgrade waits for
# the period's end, a cancel ends the subscription at the period's end (a
# tier's, a dedicated machine's or a static address's; the site opens a static
# address's cancel straight at its subscription, a subscription_cancel flow,
# which this setting allows); the card, the invoices, the email and the address
# are the customer's to see and change.
stripe_portal_args() {
  local i=0 pair
  printf '%s\n' \
    "business_profile[headline]=OpenNova monthly support tiers" \
    "business_profile[privacy_policy_url]=https://$STRIPE_DOMAIN/privacy" \
    "business_profile[terms_of_service_url]=https://$STRIPE_DOMAIN/terms" \
    "default_return_url=https://$STRIPE_WEB_HOST/me/support" \
    "features[customer_update][enabled]=true" \
    "features[customer_update][allowed_updates][]=email" \
    "features[customer_update][allowed_updates][]=address" \
    "features[invoice_history][enabled]=true" \
    "features[payment_method_update][enabled]=true" \
    "features[subscription_cancel][enabled]=true" \
    "features[subscription_cancel][mode]=at_period_end" \
    "features[subscription_cancel][proration_behavior]=none" \
    "features[subscription_update][enabled]=true" \
    "features[subscription_update][default_allowed_updates][]=price" \
    "features[subscription_update][proration_behavior]=create_prorations" \
    "features[subscription_update][schedule_at_period_end][conditions][0][type]=decreasing_item_amount" \
    "metadata[opennova]=$STRIPE_PORTAL_TAG"
  for pair in "$@"; do
    printf '%s\n' "features[subscription_update][products][$i][product]=${pair%%:*}" \
      "features[subscription_update][products][$i][prices][]=${pair#*:}"
    i=$((i + 1))
  done
}

# Set fields of vault item $1 (which exists): each further argument
# label:TYPE, its value in VAULT_VALUES[label]. A field there is overwritten,
# a missing one added; the values reach op through a template in the tmpfs,
# never an argument, and are never printed. The item is read fresh first (the
# vault's own, not the run's cache), and leaves the cache once written.
declare -A VAULT_VALUES=()
vault_set_fields() {
  local title="$1" json="$SECRETS_DIR/item.set.json" new="$SECRETS_DIR/item.set-new.json" spec label type labels=()
  shift
  vault_item_load "$title" fresh || die "reading vault item '$title' failed: $VAULT_ERR"
  (umask 077 && cp "$(vault_item_file "$title")" "$json") || die "reading vault item '$title' failed"
  for spec in "$@"; do
    label="${spec%%:*}" type="${spec#*:}"
    labels+=("$label")
    (umask 077 && OPENNOVA_FIELD_VALUE="${VAULT_VALUES[$label]}" jq --arg l "$label" --arg t "$type" \
      'if any(.fields[]?; .label == $l)
       then .fields |= map(if .label == $l then .value = env.OPENNOVA_FIELD_VALUE else . end)
       else .fields += [{label: $l, type: $t, value: env.OPENNOVA_FIELD_VALUE}] end' "$json" > "$new") \
      || die "preparing vault item '$title' failed"
    mv -f "$new" "$json"
  done
  vault_item_edit "$title" "$json" \
    || { rm -f "$json"; die "writing ${labels[*]} to vault item '$title' failed"; }
  rm -f "$json"
}

# stripe setup: make what the deployment's mode lacks, idempotently. Products and monthly
# prices by lookup key (an existing active one is kept; an amount given by a
# flag that differs makes a new price, which takes the lookup key and goes
# first in the vault field, the old one kept after it so its subscribers
# still grant), and each product's name and description brought to the
# catalog's (an item renamed: the own machine became the dedicated machine);
# the price ids into the vault item; the Customer Portal
# configuration tagged opennova=support_tiers (made, or brought to these
# settings); the webhook endpoint on https://<web host>/api/billing/webhook
# (made, its events brought up to date; a new one's signing secret goes into
# the vault's webhook_secret).
stripe_setup() {
  local mode="$STRIPE_MODE" yes=0
  declare -A amounts=() explicit=()
  local entry field lookup name desc meta cents
  while [ $# -gt 0 ]; do
    case "$1" in
      --tier1|--tier2|--tier3|--own-machine|--static-address)
        [ $# -ge 2 ] || die "$1 needs an amount in US dollars"
        case "$1" in
          --tier1) field=price_tier1 ;; --tier2) field=price_tier2 ;;
          --tier3) field=price_tier3 ;; --own-machine) field=price_own_machine ;;
          --static-address) field=price_static_address ;;
        esac
        amounts[$field]="$(usd_cents "$1" "$2")" || exit 1
        explicit[$field]=1
        shift 2 ;;
      --yes) yes=1; shift ;;
      *) die "unknown: stripe setup $1 (try: --tier1 USD, --tier2 USD, --tier3 USD, --own-machine USD, --static-address USD, --yes)" ;;
    esac
  done
  confirm_prod "make or change Stripe's live-mode objects and the stripe-live item" "$yes"
  stripe_auth "$mode"
  local livemode=false
  [ "$mode" != live ] || livemode=true
  log "stripe setup: $mode mode ($STRIPE_ITEM), the site https://$STRIPE_WEB_HOST"

  # --- the products and their monthly prices
  local vault_json current price_id product_id have want old tier_pairs=() wrote=()
  vault_json="$(item_json "$STRIPE_ITEM")"
  stripe_call GET /v1/products --data-urlencode "active=true" --data-urlencode "limit=100"
  stripe_ok "listing the products"
  local products="$SECRETS_DIR/value.stripe-products.json"
  cp "$STRIPE_REPLY" "$products"
  for entry in "${STRIPE_CATALOG[@]}"; do
    IFS='|' read -r field lookup name desc meta cents <<<"$entry"
    [ -z "${amounts[$field]:-}" ] || cents="${amounts[$field]}"
    stripe_call GET /v1/prices --data-urlencode "lookup_keys[]=$lookup" --data-urlencode "active=true"
    stripe_ok "finding the price $lookup"
    price_id="$(jq -r '.data[0].id // empty' "$STRIPE_REPLY")"
    product_id="$(jq -r '.data[0].product // empty' "$STRIPE_REPLY")"
    have="$(jq -r '.data[0].unit_amount // empty' "$STRIPE_REPLY")"
    if [ -n "$price_id" ] && { [ "$have" = "$cents" ] || [ -z "${explicit[$field]:-}" ]; }; then
      if [ "$have" = "$cents" ]; then
        log "  $field: $price_id ($lookup, $have usd cents a month)"
      else
        log "  $field: $price_id ($lookup) is $have usd cents a month, not the default $cents: kept (an amount given by its flag makes a new price)"
      fi
    else
      if [ -z "$product_id" ]; then
        product_id="$(jq -r --arg m "$meta" '[.data[]? | select((.metadata.opennova_item // "") == $m) | .id][0] // empty' "$products")"
      fi
      if [ -z "$product_id" ]; then
        stripe_call POST /v1/products --data-urlencode "name=$name" --data-urlencode "description=$desc" \
          --data-urlencode "metadata[opennova_item]=$meta" --data-urlencode "statement_descriptor=OPENNOVA"
        stripe_ok "making the product '$name'"
        product_id="$(jq -r '.id' "$STRIPE_REPLY")"
        log "  $field: made the product $product_id ($name)"
      fi
      stripe_call POST /v1/prices --data-urlencode "product=$product_id" --data-urlencode "currency=usd" \
        --data-urlencode "unit_amount=$cents" --data-urlencode "recurring[interval]=month" \
        --data-urlencode "lookup_key=$lookup" --data-urlencode "transfer_lookup_key=true" \
        --data-urlencode "metadata[opennova_item]=$meta"
      stripe_ok "making the price $lookup"
      [ "$(jq -r '.livemode' "$STRIPE_REPLY")" = "$livemode" ] \
        || die "the price $lookup came back in the other mode: the key is not a $mode mode key"
      old="$price_id"
      price_id="$(jq -r '.id' "$STRIPE_REPLY")"
      log "  $field: made $price_id ($lookup, $cents usd cents a month)${old:+, after $old (kept in the field: its subscribers still grant)}"
    fi
    # The product's name and description, as Checkout, the portal and the
    # invoices show them: the catalog's.
    stripe_call GET "/v1/products/$product_id"
    stripe_ok "reading the product $product_id"
    if [ "$(jq -r '.name // ""' "$STRIPE_REPLY")" != "$name" ] ||
        [ "$(jq -r '.description // ""' "$STRIPE_REPLY")" != "$desc" ]; then
      stripe_call POST "/v1/products/$product_id" --data-urlencode "name=$name" --data-urlencode "description=$desc"
      stripe_ok "updating the product $product_id's name and description"
      log "  $field: the product $product_id is now '$name' ($desc)"
    fi
    # The field: this price first, then the prices it held before
    # (grandfathered: their subscribers still grant).
    current="$(json_field "$vault_json" "$field")"
    want="$( { printf '%s\n' "$price_id"; tr ',' '\n' <<<"$current"; } \
      | awk '/^price_[A-Za-z0-9]+$/ && !seen[$0]++' | paste -sd, -)"
    if [ "$want" != "$current" ]; then
      VAULT_VALUES[$field]="$want"
      wrote+=("$field:STRING")
    fi
    # A dedicated machine and a static address are no tier the portal
    # switches among: each is a subscription of its own (quantity 1), bought
    # through Checkout.
    case "$field" in
      price_own_machine|price_static_address) ;;
      *) tier_pairs+=("$product_id:$price_id") ;;
    esac
  done
  rm -f "$products"
  if [ "${#wrote[@]}" -gt 0 ]; then
    vault_set_fields "$STRIPE_ITEM" "${wrote[@]}"
    log "  vault $STRIPE_ITEM: wrote ${wrote[*]%%:*}"
  else
    log "  vault $STRIPE_ITEM: the price fields already name these"
  fi

  # --- the Customer Portal configuration
  local portal_args=() portal_id
  mapfile -t portal_args < <(stripe_portal_args "${tier_pairs[@]}")
  local data_args=()
  for entry in "${portal_args[@]}"; do data_args+=(--data-urlencode "$entry"); done
  stripe_call GET /v1/billing_portal/configurations --data-urlencode "active=true" --data-urlencode "limit=100"
  stripe_ok "listing the Customer Portal configurations"
  portal_id="$(jq -r --arg t "$STRIPE_PORTAL_TAG" '[.data[]? | select((.metadata.opennova // "") == $t) | .id][0] // empty' "$STRIPE_REPLY")"
  if [ -n "$portal_id" ]; then
    stripe_call POST "/v1/billing_portal/configurations/$portal_id" "${data_args[@]}"
    stripe_ok "updating the Customer Portal configuration $portal_id"
    log "  portal: $portal_id brought to these settings (default: $(jq -r '.is_default' "$STRIPE_REPLY"))"
  else
    stripe_call POST /v1/billing_portal/configurations "${data_args[@]}"
    stripe_ok "making the Customer Portal configuration"
    portal_id="$(jq -r '.id' "$STRIPE_REPLY")"
    log "  portal: made $portal_id (default: $(jq -r '.is_default' "$STRIPE_REPLY"))"
  fi
  [ "$(jq -r '.is_default' "$STRIPE_REPLY")" = true ] \
    || log "  portal: WARNING: $portal_id is not the account's default configuration, and the server's portal sessions name none, so they get the default's settings (DEPLOY.md, \"Monthly support tiers (Stripe)\")"

  # --- the webhook endpoint
  local url="https://$STRIPE_WEB_HOST/api/billing/webhook" endpoint version missing secret ev event_args=()
  for ev in "${STRIPE_EVENTS[@]}"; do event_args+=(--data-urlencode "enabled_events[]=$ev"); done
  stripe_call GET /v1/webhook_endpoints --data-urlencode "limit=100"
  stripe_ok "listing the webhook endpoints"
  endpoint="$(jq -c --arg u "$url" '[.data[]? | select(.url == $u)][0] // empty' "$STRIPE_REPLY")"
  if [ -n "$endpoint" ]; then
    version="$(jq -r '.api_version // ""' <<<"$endpoint")"
    missing="$(jq -r --args '[$ARGS.positional[] as $e | select(.enabled_events | index($e) | not) | $e] | join(" ")' \
      "${STRIPE_EVENTS[@]}" <<<"$endpoint")"
    log "  webhook: $(jq -r '.id' <<<"$endpoint") on $url ($(jq -r '.status' <<<"$endpoint"), API $version)"
    if [ -n "$missing" ]; then
      stripe_call POST "/v1/webhook_endpoints/$(jq -r '.id' <<<"$endpoint")" "${event_args[@]}"
      stripe_ok "updating the webhook endpoint's events"
      log "  webhook: its events now include $missing"
    fi
    [ "$version" = "$STRIPE_API_VERSION" ] \
      || err "  webhook: its API version is $version, not $STRIPE_API_VERSION, and an endpoint's version never changes: delete it in Stripe (Developers, Webhooks), then stripe setup again (which writes the new signing secret)"
    log "  webhook: its signing secret is not readable again; the vault's webhook_secret is left as it is"
  else
    stripe_call POST /v1/webhook_endpoints --data-urlencode "url=$url" \
      --data-urlencode "api_version=$STRIPE_API_VERSION" \
      --data-urlencode "description=NovaWorld service: monthly support tiers (apps/novaworld_common/billing)" \
      "${event_args[@]}"
    stripe_ok "making the webhook endpoint"
    secret="$(jq -r '.secret // ""' "$STRIPE_REPLY")"
    log "  webhook: made $(jq -r '.id' "$STRIPE_REPLY") on $url ($(jq -r '.enabled_events | length' "$STRIPE_REPLY") events, API $(jq -r '.api_version' "$STRIPE_REPLY"))"
    rm -f "$STRIPE_REPLY"
    stripe_field_ok webhook_secret "$secret" "$mode" || { secret=""; die "the new endpoint's reply holds no signing secret: delete the endpoint in Stripe and run stripe setup again"; }
    VAULT_VALUES[webhook_secret]="$secret"
    secret=""
    vault_set_fields "$STRIPE_ITEM" webhook_secret:CONCEALED
    VAULT_VALUES[webhook_secret]=""
    log "  vault $STRIPE_ITEM: wrote webhook_secret"
  fi
  log "done. Next: ./deploy/run.sh$DEP_FLAG stripe check, then secrets sync and app converge;"
  log "then Stripe's \"Send test webhook\" on the endpoint must answer 2xx."
}

# stripe check: the deployment's vault item and its objects in Stripe, read alone
# (GET), nothing printed that is a secret. Fails (exit 1) on what keeps billing
# from working.
stripe_check() {
  local mode="$STRIPE_MODE"
  [ $# -eq 0 ] || die "unknown: stripe check $1 (the mode is the deployment's: --deployment staging or prod)"
  stripe_auth "$mode"
  local livemode=false rc=0 vault_json value entry field lookup name desc meta cents id first bad problems=()
  [ "$mode" != live ] || livemode=true
  log "stripe check: $mode mode ($STRIPE_ITEM), the site https://$STRIPE_WEB_HOST (no value is printed)"
  log "  secret_key    OK ($mode mode form)"
  vault_json="$(item_json "$STRIPE_ITEM")"
  value="$(json_field "$vault_json" webhook_secret)"
  if [ -z "$value" ] || [ "$value" = REPLACE_ME ]; then
    err "  webhook_secret    FAILED: a placeholder (stripe setup writes it when it makes the endpoint)"; rc=1
  elif stripe_field_ok webhook_secret "$value" "$mode"; then
    log "  webhook_secret    OK (whsec_ form)"
  else
    err "  webhook_secret    FAILED: malformed (whsec_...)"; rc=1
  fi
  value=""

  # The prices: the field's first is sold (active, monthly, this mode, the
  # lookup key's); the rest still grant (they must exist).
  for entry in "${STRIPE_CATALOG[@]}"; do
    IFS='|' read -r field lookup name desc meta cents <<<"$entry"
    value="$(json_field "$vault_json" "$field")"
    if [ -z "$value" ] || [ "$value" = REPLACE_ME ]; then
      err "  $field    FAILED: a placeholder (stripe setup writes it)"; rc=1; continue
    fi
    if ! stripe_field_ok "$field" "$value" "$mode"; then
      err "  $field    FAILED: malformed (price_... or a comma list of them)"; rc=1; continue
    fi
    first="${value%%,*}"
    problems=()
    while IFS= read -r id; do
      stripe_call GET "/v1/prices/$id"
      if [ "$STRIPE_HTTP" != 200 ]; then
        problems+=("$id: Stripe answered $STRIPE_HTTP ($(stripe_error))")
        continue
      fi
      [ "$id" = "$first" ] || continue
      bad="$(jq -r --argjson live "$livemode" '
        [ (if .active then empty else "inactive" end),
          (if .type == "recurring" and (.recurring.interval // "") == "month" and (.recurring.interval_count // 1) == 1 then empty else "not monthly" end),
          (if .livemode == $live then empty else "of the other mode" end) ] | join(", ")' "$STRIPE_REPLY")"
      [ -z "$bad" ] || problems+=("$id: $bad")
      cents="$(jq -r '.unit_amount' "$STRIPE_REPLY")"
    done < <(tr ',' '\n' <<<"$value")
    if [ "${#problems[@]}" -gt 0 ]; then
      err "  $field    FAILED: $(printf '%s; ' "${problems[@]}" | sed 's/; $//')"; rc=1; continue
    fi
    stripe_call GET /v1/prices --data-urlencode "lookup_keys[]=$lookup" --data-urlencode "active=true"
    id="$(jq -r '.data[0].id // empty' "$STRIPE_REPLY" 2>/dev/null)"
    if [ "$id" != "$first" ]; then
      err "  $field    FAILED: sells $first, but the lookup key $lookup names ${id:-no active price} (stripe setup)"; rc=1; continue
    fi
    log "  $field    OK ($first, $cents usd cents a month$( [ "$value" = "$first" ] || printf ', and %s grandfathered' "${value#*,}"))"
  done

  # The Customer Portal configuration.
  stripe_call GET /v1/billing_portal/configurations --data-urlencode "active=true" --data-urlencode "limit=100"
  if [ "$STRIPE_HTTP" != 200 ]; then
    err "  portal    FAILED: Stripe answered $STRIPE_HTTP ($(stripe_error))"; rc=1
  else
    id="$(jq -r --arg t "$STRIPE_PORTAL_TAG" '[.data[]? | select((.metadata.opennova // "") == $t) | .id][0] // empty' "$STRIPE_REPLY")"
    if [ -z "$id" ]; then
      err "  portal    FAILED: no active configuration tagged opennova=$STRIPE_PORTAL_TAG (stripe setup)"; rc=1
    elif [ "$(jq -r --arg i "$id" '.data[] | select(.id == $i) | .is_default' "$STRIPE_REPLY")" = true ]; then
      log "  portal    OK ($id, the default)"
    else
      log "  portal    OK ($id); WARNING: not the default, and the server's portal sessions name no configuration, so they get the default's settings ($(jq -r '[.data[] | select(.is_default) | .id][0] // "none"' "$STRIPE_REPLY"))"
    fi
  fi

  # The webhook endpoint.
  local url="https://$STRIPE_WEB_HOST/api/billing/webhook" endpoint missing
  stripe_call GET /v1/webhook_endpoints --data-urlencode "limit=100"
  if [ "$STRIPE_HTTP" != 200 ]; then
    err "  webhook    FAILED: Stripe answered $STRIPE_HTTP ($(stripe_error))"; rc=1
  else
    endpoint="$(jq -c --arg u "$url" '[.data[]? | select(.url == $u)][0] // empty' "$STRIPE_REPLY")"
    if [ -z "$endpoint" ]; then
      err "  webhook    FAILED: no endpoint on $url (stripe setup)"; rc=1
    else
      missing="$(jq -r --args '[$ARGS.positional[] as $e | select(.enabled_events | index($e) | not) | $e] | join(" ")' \
        "${STRIPE_EVENTS[@]}" <<<"$endpoint")"
      problems=()
      [ "$(jq -r '.status' <<<"$endpoint")" = enabled ] || problems+=("disabled")
      [ "$(jq -r '.api_version // ""' <<<"$endpoint")" = "$STRIPE_API_VERSION" ] \
        || problems+=("API $(jq -r '.api_version // "none"' <<<"$endpoint"), not $STRIPE_API_VERSION")
      [ -z "$missing" ] || problems+=("lacks $missing")
      if [ "${#problems[@]}" -gt 0 ]; then
        err "  webhook    FAILED: $(jq -r '.id' <<<"$endpoint") on $url: $(printf '%s; ' "${problems[@]}" | sed 's/; $//')"; rc=1
      else
        log "  webhook    OK ($(jq -r '.id' <<<"$endpoint") on $url, API $STRIPE_API_VERSION, ${#STRIPE_EVENTS[@]} events)"
      fi
    fi
  fi
  [ "$rc" -eq 0 ] || die "stripe check: one or more checks failed (stripe setup makes what is missing)"
  log "stripe check: all OK. Stripe's \"Send test webhook\" on the endpoint must answer 2xx."
}

cmd_stripe() {
  local action="${1:-}"
  shift || true
  case "$action" in
    check) stripe_check "$@" ;;
    setup) stripe_setup "$@" ;;
    *) die "unknown: stripe $action (try: check|setup; the mode is the deployment's)" ;;
  esac
}
