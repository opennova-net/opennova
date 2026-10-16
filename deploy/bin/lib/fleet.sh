# shellcheck shell=bash
# on-deploy's hosted game servers: the machines (`fleet`) and the game data
# snapshots (`hosted`).
# deploy/bin/on-deploy sources this file before it runs a command: it defines
# functions and their constants alone, and runs nothing itself. The helpers
# every command shares (log, die, the deployment, the vault, AWS) are
# on-deploy's own.

# ---------------------------------------------------------------------------
# Hosted game servers (ADR 0053; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------

# Where a data set's game folder is mounted into this container for
# `hosted data-snapshot` (run.sh and run.ps1 mount HOSTED_DATA_DIR there).
HOSTED_DATA_MOUNT=/hosted-data
HOSTED_DATA_IMAGE=opennova-hosted-data:local

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

# fleet status: every machine this deployment tagged (the EC2 view, which
# answers while the service is down), then the service's own view.
fleet_status() {
  [ $# -eq 0 ] || die "unknown: fleet status $1 (the deployment is --deployment's)"
  aws_auth
  infra_env_load
  local tag
  tag="$(hosted_deployment_tag)"
  log "machines tagged opennova:deployment=$tag (EC2):"
  # shellcheck disable=SC2016  # a JMESPath literal, not a shell expansion
  aws ec2 describe-instances \
    --filters "Name=tag:opennova:hosted,Values=1" "Name=tag:opennova:deployment,Values=$tag" \
    --query 'Reservations[].Instances[].{machine:InstanceId,kind:Tags[?Key==`opennova:machine-kind`]|[0].Value,id:Tags[?Key==`opennova:machine-id`]|[0].Value,state:State.Name,type:InstanceType,public:PublicIpAddress,launched:LaunchTime}' \
    --output table
  log "the service's fleet ($FLEET_VIEW_API):"
  admin_api GET "$FLEET_VIEW_API" \
    || err "the service did not answer (app status, app logs web)"
}

# fleet bake-image: the machines' image, baked. A one-off builder instance
# (FLEET_BUILDER_TYPE, no instance role, the machines' security group, public
# subnet a) boots Canonical's newest Ubuntu 24.04 with deploy/host/fleet-image.sh
# as its user data, which installs what every machine's first boot installs and
# pulls the current release's serve and hostd images, then powers off. Its image
# (tagged opennova:fleet-image=<deployment tag>, which the workers' launch
# policy admits) goes into the fleet's image parameter, which the launch
# templates resolve at each launch: no plan changes. The builder is ended either
# way; of this deployment's baked images the newest two are kept (the current
# and the one before, to point the parameter back at), the rest deregistered with
# their snapshots.
FLEET_BUILDER_TYPE=t3a.small
fleet_bake_image() {
  [ $# -eq 0 ] || die "unknown: fleet bake-image $1"
  aws_auth
  infra_env_load
  [ "${INFRA[ONNET_HOSTED_PROVIDER]:-none}" = ec2 ] \
    || die "hosting is off (no 'hosted' vault item, infra apply): there is no fleet to bake an image for"
  local param sg subnet tag base current manifest pull user_data name tags iid ami state waited=0
  param="$(infra_get OPENNOVA_FLEET_IMAGE_PARAMETER)"
  sg="$(infra_get OPENNOVA_FLEET_SECURITY_GROUP)"
  subnet="$(infra_get ONNET_HOSTED_EC2_SUBNETS)"
  subnet="${subnet%%,*}"
  tag="$(hosted_deployment_tag)"
  base="$(aws ec2 describe-images --owners 099720109477 \
            --filters 'Name=name,Values=ubuntu/images/*ubuntu-noble-24.04-amd64-server-*' \
                      Name=architecture,Values=x86_64 Name=root-device-type,Values=ebs Name=state,Values=available \
            --query 'sort_by(Images, &CreationDate)[-1].ImageId' --output text)" \
    || die "finding Canonical's newest Ubuntu 24.04 image failed"
  [[ "$base" =~ ^ami-[0-9a-f]{8,17}$ ]] || die "no Ubuntu 24.04 image found ('$base')"
  manifest="$(release_current_manifest)"
  current="$(jq -r '.id // "none"' <<<"$manifest")"
  pull="$(jq -r '[.images.serve // empty, .images.hostd // empty] | unique | join(" ")' <<<"$manifest")"
  [[ "$pull" =~ ^[A-Za-z0-9./:@\ _-]*$ ]] || die "the release names an image reference the builder cannot take: $pull"
  user_data="$SECRETS_DIR/fleet-image.sh.rendered"
  sed "s|@OPENNOVA_PULL_IMAGES@|$pull|" "$ROOT/deploy/host/fleet-image.sh" > "$user_data"
  name="opennova-fleet-$tag-$(date -u +%Y%m%dT%H%M%SZ)"
  tags="{Key=opennova:deployment,Value=$tag},{Key=opennova:fleet-builder,Value=1},{Key=Name,Value=$name-builder}"
  log "baking the $DEP fleet image from $base (release $current: ${pull:-no image to pull})…"
  iid="$(aws ec2 run-instances --image-id "$base" --instance-type "$FLEET_BUILDER_TYPE" \
          --subnet-id "$subnet" --security-group-ids "$sg" \
          --metadata-options HttpEndpoint=enabled,HttpTokens=required,HttpPutResponseHopLimit=1 \
          --instance-initiated-shutdown-behavior stop \
          --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=8,VolumeType=gp3,Encrypted=true,DeleteOnTermination=true}' \
          --user-data "file://$user_data" \
          --tag-specifications "ResourceType=instance,Tags=[$tags]" "ResourceType=volume,Tags=[$tags]" \
                               "ResourceType=network-interface,Tags=[$tags]" \
          --query 'Instances[0].InstanceId' --output text)" \
    || { rm -f "$user_data"; die "the builder did not launch"; }
  rm -f "$user_data"
  log "builder $iid: it powers off when baked (a few minutes; 20 at most)…"
  while :; do
    state="$(aws ec2 describe-instances --instance-ids "$iid" \
               --query 'Reservations[0].Instances[0].State.Name' --output text 2>/dev/null || echo unknown)"
    [ "$state" != stopped ] || break
    if [ "$waited" -ge 1200 ] || [ "$state" = terminated ]; then
      aws ec2 terminate-instances --instance-ids "$iid" >/dev/null 2>&1 || true
      die "the builder did not finish (state $state after ${waited}s); it is ended and no image was made. Why: ./deploy/run.sh$DEP_FLAG aws ec2 get-console-output --instance-id $iid --latest --output text"
    fi
    sleep 15
    waited=$((waited + 15))
  done
  ami="$(aws ec2 create-image --instance-id "$iid" --name "$name" \
          --description "OpenNova $DEP fleet machine image (deploy/host/fleet-image.sh, from $base)" \
          --tag-specifications \
            "ResourceType=image,Tags=[{Key=opennova:deployment,Value=$tag},{Key=opennova:fleet-image,Value=$tag},{Key=Name,Value=$name}]" \
            "ResourceType=snapshot,Tags=[{Key=opennova:deployment,Value=$tag},{Key=opennova:fleet-image,Value=$tag},{Key=Name,Value=$name}]" \
          --query ImageId --output text)" \
    || { aws ec2 terminate-instances --instance-ids "$iid" >/dev/null 2>&1 || true; die "making the image of $iid failed; the builder is ended"; }
  log "image $ami: waiting until it is available…"
  if ! aws ec2 wait image-available --image-ids "$ami" && ! aws ec2 wait image-available --image-ids "$ami"; then
    aws ec2 terminate-instances --instance-ids "$iid" >/dev/null 2>&1 || true
    die "image $ami did not become available in 20 minutes; the fleet still launches from what $param names"
  fi
  aws ec2 terminate-instances --instance-ids "$iid" >/dev/null \
    || err "ending the builder $iid failed: end it by hand (aws ec2 terminate-instances --instance-ids $iid)"
  aws ssm put-parameter --name "$param" --type String --data-type aws:ec2:image --value "$ami" --overwrite >/dev/null \
    || die "pointing $param at $ami failed; the fleet still launches from what it names"
  log "new machines launch from $ami ($param)"
  local old snaps snap
  for old in $(aws ec2 describe-images --owners self --filters "Name=tag:opennova:fleet-image,Values=$tag" \
                 --query 'sort_by(Images, &CreationDate)[].ImageId' --output text | tr '\t' '\n' | sed '/^$/d' | head -n -2); do
    snaps="$(aws ec2 describe-images --image-ids "$old" --query 'Images[0].BlockDeviceMappings[].Ebs.SnapshotId' --output text)" || continue
    aws ec2 deregister-image --image-id "$old" >/dev/null || { err "deregistering $old failed"; continue; }
    for snap in $snaps; do
      [ "$snap" = None ] || aws ec2 delete-snapshot --snapshot-id "$snap" >/dev/null || err "deleting $snap failed"
    done
    log "  $old (an older image) deregistered"
  done
  log "running machines keep their image: ./deploy/run.sh$DEP_FLAG fleet roll moves their servers onto new ones"
}

cmd_fleet() {
  local action="${1:-status}"
  shift || true
  case "$action" in
    status) fleet_status "$@" ;;
    bake-image) fleet_bake_image "$@" ;;
    drain|end)
      take_yes "$@"
      [ "${#OPT_REST[@]}" -eq 1 ] || die "fleet $action needs one MACHINE (fleet status lists them)"
      local machine
      machine="$(machine_id "${OPT_REST[0]}")" || exit 1
      [ "$action" = drain ] || confirm_prod "end machine $machine (its servers move off first)" "$OPT_YES"
      aws_auth
      admin_api POST "$FLEET_API/machines/$machine/$action" || die "the service refused or did not answer (app logs web)" ;;
    roll)
      take_yes "$@"
      [ "${#OPT_REST[@]}" -eq 0 ] || die "unknown: fleet roll ${OPT_REST[0]} (try: --yes)"
      confirm_prod "roll every machine onto new ones (their servers move, one machine at a time)" "$OPT_YES"
      aws_auth
      admin_api POST "$FLEET_API/roll" || die "the service refused or did not answer (app logs web)" ;;
    *) die "unknown: fleet $action (try: status|drain|end|roll|bake-image)" ;;
  esac
}

# hosted data-snapshot --data-set SLUG: one data set of the hosted servers'
# game data, once, as a private encrypted EBS snapshot. The data set's game
# folder is mounted here (HOSTED_DATA_MOUNT) read-only; only the files
# opennova-serve reads are taken (apps/serve/README.md, "Container image"):
#   jop_2_consumer       a Joint Operations Combined Arms install: the three
#                        base archives, Escalation's archive pair and its
#                        version.txt (required), score.ini and _NSTMOUT.TXT
#                        when present;
#   dfx2_consumer        a Delta Force Xtreme 2 install: the three base
#                        archives (required), score.ini and _NSTMOUT.TXT when
#                        present;
#   opennova_0_consumer  OpenNova's own data: the three base archives.
# They stream as a tar into the helper image (deploy/hosted-data), built on this
# docker, which makes an ext4 image of them and writes it into a new snapshot
# through the EBS direct APIs: no machine, no bucket, no SSH. The snapshot is the
# only AWS resource it makes, and it deletes it on any failure; the image and the
# staging copy live in the helper's container alone (--rm). Prints the snapshot id.
hosted_data_snapshot() {
  local data_set="" what dir="$HOSTED_DATA_MOUNT" rel found tag
  while [ $# -gt 0 ]; do
    case "$1" in
      --data-set) [ $# -ge 2 ] || die "--data-set needs a value"; data_set="$2"; shift 2 ;;
      *) die "unknown: hosted data-snapshot $1 (try: --data-set SLUG)" ;;
    esac
  done
  [ -n "$data_set" ] \
    || die "hosted data-snapshot needs --data-set, one of: ${HOSTED_DATA_SETS[*]} (DEPLOY.md, \"Hosted game servers\")"
  hosted_known_data_set "$data_set" \
    || die "--data-set '$data_set' is no data set; one of: ${HOSTED_DATA_SETS[*]}"
  # The data set's files: the required ones, then the ones taken when present.
  local required=(resource.pff localres.pff language.pff) optional=()
  case "$data_set" in
    jop_2_consumer)
      what="a Joint Operations Combined Arms install (with expansion/jox01)"
      required+=(expansion/jox01/jox01.pff expansion/jox01/jox01L.pff expansion/jox01/version.txt)
      optional=(score.ini _nstmout.txt) ;;
    dfx2_consumer)
      what="a Delta Force Xtreme 2 install"
      optional=(score.ini _nstmout.txt) ;;
    opennova_0_consumer)
      what="OpenNova's data (its three base archives)" ;;
  esac
  if [ ! -d "$dir" ] || [ -z "$(ls -A "$dir" 2>/dev/null)" ]; then
    die "no game folder at $dir: run HOSTED_DATA_DIR='<the folder: $what>' ./deploy/run.sh$DEP_FLAG hosted data-snapshot --data-set $data_set (DEPLOY.md, \"Hosted game servers\")"
  fi
  local files=()
  for rel in "${required[@]}"; do
    found="$(cd "$dir" && find . -type f -ipath "./$rel" | head -n 1)"
    [ -n "$found" ] || die "$dir has no $rel: mount $what"
    files+=("${found#./}")
  done
  for rel in ${optional[@]+"${optional[@]}"}; do
    found="$(cd "$dir" && find . -maxdepth 1 -type f -iname "$rel" | head -n 1)"
    [ -z "$found" ] || files+=("${found#./}")
  done
  aws_auth
  tag="$(hosted_deployment_tag)"
  log "the $data_set data snapshot (account $ACCOUNT_ID) takes, from $dir:"
  for rel in "${files[@]}"; do log "  $rel"; done
  log "building the helper image (deploy/hosted-data)…"
  docker build -q -t "$HOSTED_DATA_IMAGE" "$ROOT/deploy/hosted-data" >/dev/null \
    || die "the helper image did not build"
  log "making the snapshot (tagged opennova:deployment=$tag, opennova:data-set=$data_set); a GiB takes a few minutes…"
  # The deployment's role's short-lived credentials and its region alone, for
  # the helper, in the tmpfs: the snapshot is made in the deployment's account.
  local aws_env="$SECRETS_DIR/aws.env" snapshot rc=0
  (umask 077 && printf 'AWS_ACCESS_KEY_ID=%s\nAWS_SECRET_ACCESS_KEY=%s\nAWS_SESSION_TOKEN=%s\nAWS_REGION=%s\nAWS_DEFAULT_REGION=%s\n' \
    "${AWS_ACCESS_KEY_ID:-}" "${AWS_SECRET_ACCESS_KEY:-}" "${AWS_SESSION_TOKEN:-}" "$REGION" "$REGION" > "$aws_env")
  snapshot="$( (cd "$dir" && tar -cf - "${files[@]}") \
    | docker run --rm -i --env-file "$aws_env" "$HOSTED_DATA_IMAGE" --data-set "$data_set" --deployment "$tag")" || rc=$?
  rm -f "$aws_env"
  [ "$rc" -eq 0 ] \
    || die "the snapshot was not made (the helper deletes one it started; check: ./deploy/run.sh$DEP_FLAG aws ec2 describe-snapshots --owner-ids self --filters Name=tag:opennova:hosted-data,Values=1)"
  snapshot="$(printf '%s\n' "$snapshot" | tail -n 1)"
  case "$snapshot" in
    snap-*) ;;
    *) die "the helper printed no snapshot id ('$snapshot')" ;;
  esac
  log "made $snapshot for $data_set. Next (DEPLOY.md, \"Hosted game servers\"):"
  log "  1. in vault '$VAULT', item '$HOSTED_ITEM', the field data_snapshot_id_$data_set = $snapshot"
  log "     (one field per data set; delete the snapshot it replaces once the infra no longer names it)"
  log "  2. ./deploy/run.sh$DEP_FLAG infra plan, then infra apply"
  log "  3. ./deploy/run.sh$DEP_FLAG app deploy --serve-tag <serve tag>"
  printf '%s\n' "$snapshot"
}

cmd_hosted() {
  local action="${1:-}"
  shift || true
  case "$action" in
    data-snapshot) hosted_data_snapshot "$@" ;;
    status) die "hosted status is fleet status now" ;;
    *) die "unknown: hosted $action (try: data-snapshot; the machines are fleet status)" ;;
  esac
}
