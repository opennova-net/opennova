# shellcheck shell=bash
# on-deploy's database: its role and database, psql, a reset and a snapshot
# (`db`), and the dumps (`backup`), one-off tasks of the backup task definition.
# deploy/bin/on-deploy sources this file before it runs a command: it defines
# functions and their constants alone, and runs nothing itself. The helpers
# every command shares (log, die, the deployment, the vault, AWS) are
# on-deploy's own.

# ---------------------------------------------------------------------------
# db (PostgreSQL 17 on RDS, infra/aws/database.tf) and backup: one-off tasks of
# the backup task definition (infra/aws/backup.tf)
# ---------------------------------------------------------------------------

# The master password, lent to the backup task for one command: a
# SecureString, /opennova/<deployment>/bootstrap/db-master-password, which the
# task reads once (its role may) and cleanup deletes when the command ends.
# Vault -> tmpfs file -> put-parameter file://: never an argument, never
# printed.
db_master_param_put() {
  require_op
  local f="$SECRETS_DIR/value.master"
  op read --no-newline "op://$VAULT/$RDS_ITEM/master_password" > "$f" 2>/dev/null \
    || { rm -f "$f"; die "reading op://$VAULT/$RDS_ITEM/master_password failed (secrets init makes it)"; }
  chmod 600 "$f"
  DB_MASTER_PARAM="$SSM_PREFIX/bootstrap/db-master-password"
  aws ssm put-parameter --name "$DB_MASTER_PARAM" --type SecureString --tier Standard \
    --value "file://$f" --overwrite >/dev/null \
    || { rm -f "$f"; DB_MASTER_PARAM=""; die "writing $SSM_PREFIX/bootstrap/db-master-password failed"; }
  rm -f "$f"
}

# The database's names from infra.env, checked before they go into a script.
db_names() {
  infra_env_load
  DB_HOST="$(infra_get OPENNOVA_DB_HOST)"
  DB_PORT="$(infra_get OPENNOVA_DB_PORT)"
  DB_NAME="$(infra_get OPENNOVA_DB_NAME)"
  DB_USER="$(infra_get OPENNOVA_DB_USER)"
  DB_MASTER="$(infra_get OPENNOVA_DB_MASTER_USER)"
  [[ "$DB_HOST" =~ ^[a-z0-9.-]+$ ]] || die "infra.env's OPENNOVA_DB_HOST '$DB_HOST' is no hostname"
  [[ "$DB_PORT" =~ ^[0-9]+$ ]] || die "infra.env's OPENNOVA_DB_PORT '$DB_PORT' is no port"
  local n
  for n in "$DB_NAME" "$DB_USER" "$DB_MASTER"; do
    [[ "$n" =~ ^[a-z_][a-z0-9_]{0,62}$ ]] || die "infra.env names the database identifier '$n', which is no plain name"
  done
}

# The SQL that makes the service's role and its database, or sets the role's
# password again: idempotent. The database is SQL_ASCII with C collation, the
# only one the programs accept (retail hosts send cp1252 bytes, which they store
# byte for byte; their boot refuses any other). The password comes from the
# task's environment (\getenv), never from the script's text.
db_bootstrap_sql() {
  cat <<SQL
\\getenv app_password NOVAWORLD_PASSWORD
SELECT format('CREATE ROLE %I LOGIN PASSWORD %L', '$DB_USER', :'app_password')
  WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = '$DB_USER') \\gexec
SELECT format('ALTER ROLE %I WITH LOGIN PASSWORD %L', '$DB_USER', :'app_password') \\gexec
GRANT $DB_USER TO CURRENT_USER;
SELECT format('CREATE DATABASE %I OWNER %I TEMPLATE template0 ENCODING %L LC_COLLATE %L LC_CTYPE %L',
              '$DB_NAME', '$DB_USER', 'SQL_ASCII', 'C', 'C')
  WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = '$DB_NAME') \\gexec
REVOKE ALL ON DATABASE $DB_NAME FROM PUBLIC;
GRANT CONNECT, TEMPORARY ON DATABASE $DB_NAME TO $DB_USER;
\\pset footer off
SELECT datname AS database, pg_get_userbyid(datdba) AS owner, pg_encoding_to_char(encoding) AS encoding,
       datcollate AS lc_collate, datctype AS lc_ctype
  FROM pg_database WHERE datname = '$DB_NAME';
SQL
}

# A one-off task's command running SQL ($1) as the master user against the
# postgres database: the master password from the lent parameter (the task's
# role reads it), the service role's password from the task's
# DATABASE_PASSWORD secret, both into psql's environment alone. The SQL rides
# base64-encoded in the command (it holds names, never a password).
# shellcheck disable=SC2016  # the task's shell expands these
db_master_command() {
  local b64 script
  b64="$(printf '%s\n' "$1" | base64 -w0)"
  script='set -euo pipefail
PGPASSWORD="$(aws ssm get-parameter --name "'"$SSM_PREFIX"'/bootstrap/db-master-password" --with-decryption --query Parameter.Value --output text)"
NOVAWORLD_PASSWORD="$DATABASE_PASSWORD"
export PGPASSWORD NOVAWORLD_PASSWORD
printf %s '"$b64"' | base64 -d | psql "host='"$DB_HOST"' port='"$DB_PORT"' dbname=postgres user='"$DB_MASTER"' sslmode=verify-full sslrootcert=/app/rds-global-bundle.pem" -X -q -v ON_ERROR_STOP=1 -f -'
  jq -nc --arg s "$script" '["bash", "-c", $s]'
}

# A one-off task's command running SQL ($1) as the service's role on its
# database, with psql's further arguments ($2..).
# shellcheck disable=SC2016  # the task's shell expands these
db_psql_command() {
  local b64 script
  b64="$(printf '%s\n' "$1" | base64 -w0)"
  shift
  script='set -euo pipefail
export PGPASSWORD="$DATABASE_PASSWORD"
printf %s '"$b64"' | base64 -d | psql "$DATABASE_URL" -X -v ON_ERROR_STOP=1 '"$*"' -f -'
  jq -nc --arg s "$script" '["bash", "-c", $s]'
}

db_bootstrap() {
  take_yes "$@"
  [ "${#OPT_REST[@]}" -eq 0 ] || die "unknown: db bootstrap ${OPT_REST[0]} (try: --yes)"
  confirm_prod "make the service's role and database, or set the role's password to the vault's" "$OPT_YES"
  aws_auth
  db_names
  log "db bootstrap: role $DB_USER and database $DB_NAME on $DB_HOST (as $DB_MASTER), in a one-off task…"
  db_master_param_put
  one_off_task "$(db_master_command "$(db_bootstrap_sql)")" 600 \
    || die "db bootstrap failed (secrets sync first: the role's password is the DATABASE_PASSWORD secret)"
  log "db bootstrap complete. Deploy (or app restart) to connect with it."
}

db_psql() {
  aws_auth
  infra_env_load
  if [ $# -eq 0 ]; then
    log "psql as the service's role, in a one-off task over ECS Exec (\\q ends it)…"
    one_off_task '["sleep", "3600"]' 600 exec
    local task="$TASK_ARN" rc=0
    # shellcheck disable=SC2016  # the task's shell expands these
    ecs_exec "$task" backup "$(proc1_env DATABASE_URL url)$(proc1_env DATABASE_PASSWORD PGPASSWORD)"'export PGPASSWORD
exec psql "$url" </dev/tty >/dev/tty 2>&1' || rc=$?
    aws ecs stop-task --cluster "$(ecs_cluster)" --task "$task" --reason "opennova toolbox: db psql ended" >/dev/null 2>&1 || true
    return "$rc"
  fi
  local sql=""
  case "$1" in
    -c) [ $# -eq 2 ] || die "db psql -c needs one SQL argument"; sql="$2" ;;
    -f) [ $# -eq 2 ] || die "db psql -f needs one FILE"; [ -f "$2" ] || die "no file $2"; sql="$(cat "$2")" ;;
    *) die "unknown: db psql $1 (try: [-c SQL | -f FILE])" ;;
  esac
  one_off_task "$(db_psql_command "$sql")" 600
}

# The count of subscriptions that grant hosting (active, trialing, past_due),
# 0 while the billing table does not exist.
DB_GUARD_SQL="SELECT to_regclass('public.billing_subscriptions') IS NOT NULL AS has_billing \\gset
\\if :has_billing
SELECT 'granting=' || count(*) FROM billing_subscriptions WHERE status IN ('active', 'trialing', 'past_due');
\\else
SELECT 'granting=0';
\\endif"

db_reset() {
  local confirmed=0 orphan=0 backup=1 yes=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --yes-drop-database) confirmed=1; shift ;;
      --orphan-subscriptions) orphan=1; shift ;;
      --no-backup) backup=0; shift ;;
      --yes) yes=1; shift ;;
      *) die "unknown: db reset-db $1 (try: --yes-drop-database [--orphan-subscriptions] [--no-backup] [--yes])" ;;
    esac
  done
  aws_auth
  db_names
  log "checking for subscriptions that grant hosting…"
  local out granting
  out="$(one_off_task "$(db_psql_command "$DB_GUARD_SQL" -q -At)" 600)" \
    || die "the check failed; nothing was changed (a database never bootstrapped wants db bootstrap, not a reset)"
  granting="$(sed -n 's/^granting=\([0-9][0-9]*\)$/\1/p' <<<"$out" | tail -n 1)"
  [ -n "$granting" ] || die "the check printed no count; nothing was changed"
  if [ "$granting" -gt 0 ] && [ "$orphan" -eq 0 ]; then
    die "$granting subscription(s) (active, trialing or past_due) grant hosting: once billing is live the database is durable. Refusing. (--orphan-subscriptions resets anyway: their customers keep paying with no account linked here, so cancel them in Stripe first)"
  fi
  log "db reset-db on $DEP will:"
  if [ "$backup" -eq 1 ]; then
    log "  1. dump the database to S3 (as backup now); a failed dump stops here, nothing changed"
  else
    log "  1. take NO dump (--no-backup)"
  fi
  log "  2. stop every program (app down: the site's API and the game service are down until step 4)"
  log "  3. drop the database $DB_NAME (WITH FORCE) and make it again, empty (SQL_ASCII, C collation, owned by $DB_USER)"
  log "  4. start every program again (app up): the first to boot applies every migration"
  log "  ($granting subscription(s) grant hosting now)"
  [ "$confirmed" -eq 1 ] || die "refusing to drop the database without --yes-drop-database"
  confirm_prod "drop prod's database and make it again, empty" "$yes"
  if [ "$backup" -eq 1 ]; then
    log "[reset-db] dumping the database"
    one_off_task '["/backup/backup.sh"]' 1800 || die "the dump failed; nothing was changed"
  fi
  log "[reset-db] stopping every program"
  programs_down
  db_master_param_put
  if ! one_off_task "$(db_master_command "DROP DATABASE IF EXISTS $DB_NAME WITH (FORCE);
$(db_bootstrap_sql)")" 1800; then
    err "[reset-db] the reset failed; starting the programs again"
    programs_up || true
    die "db reset-db failed; its output above says where"
  fi
  log "[reset-db] starting every program on the empty database"
  programs_up || die "the programs did not all come back (app status, app logs)"
  log "reset-db complete. Every account is gone: make the first site admin again (DEPLOY.md, ONNET_BOOTSTRAP_ADMIN)."
}

db_snapshot() {
  [ $# -eq 0 ] || die "unknown: db snapshot $1"
  aws_auth
  infra_env_load
  local id snap
  id="$(infra_get OPENNOVA_DB_IDENTIFIER)"
  snap="$id-manual-$(date -u +%Y%m%d-%H%M%S)"
  aws rds create-db-snapshot --db-instance-identifier "$id" --db-snapshot-identifier "$snap" \
    --query 'DBSnapshot.[DBSnapshotIdentifier,Status]' --output text \
    || die "the snapshot was not started"
  log "snapshot $snap started (minutes): ./deploy/run.sh$DEP_FLAG aws rds wait db-snapshot-available --db-snapshot-identifier $snap"
}

cmd_db() {
  local action="${1:-}"
  shift || true
  case "$action" in
    bootstrap) db_bootstrap "$@" ;;
    psql)      db_psql "$@" ;;
    reset-db)  db_reset "$@" ;;
    snapshot)  db_snapshot "$@" ;;
    *) die "unknown: db $action (try: bootstrap|psql|reset-db|snapshot)" ;;
  esac
}

backup_restore() {
  local key="" confirmed=0 yes=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --yes-overwrite-database) confirmed=1; shift ;;
      --yes) yes=1; shift ;;
      -*) die "unknown: backup restore $1 (try: KEY --yes-overwrite-database [--yes])" ;;
      *) [ -z "$key" ] || die "backup restore takes one KEY"; key="$1"; shift ;;
    esac
  done
  [ -n "$key" ] || die "backup restore needs the dump's KEY (backup list)"
  if ! [[ "$key" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]*$ ]] || [[ "$key" == *..* ]]; then die "'$key' is no key"; fi
  aws_auth
  infra_env_load
  local prefix
  prefix="$(infra_get BACKUP_PREFIX)"
  [[ "$key" == "$prefix/"* ]] || die "$key is not under $prefix/, the deployment's own dumps, which alone its backup task reads (another deployment's comes in by backup import first: DEPLOY.md, \"The restore drill\")"
  log "backup restore on $DEP: stop every program, restore s3://$(infra_get BACKUP_BUCKET)/$key over the database (a one-off task), start them again"
  [ "$confirmed" -eq 1 ] || die "refusing to overwrite the database without --yes-overwrite-database"
  confirm_prod "overwrite prod's database with $key" "$yes"
  log "[restore] stopping every program"
  programs_down
  if ! one_off_task "$(jq -nc --arg k "$key" '["/backup/restore.sh", $k]')" 1800; then
    err "[restore] the restore failed; starting the programs again"
    programs_up || true
    die "the restore failed; its output above says where"
  fi
  log "[restore] starting every program"
  programs_up || die "the programs did not all come back (app status, app logs)"
  log "restore complete: $key"
}

# Where an import lands in the deployment's dumps (infra.env's bucket and
# prefix): <prefix>/$1 when it names the dump (--as), else
# <prefix>/<prefix>-import-<time>.dump.
backup_import_target() {
  local name="${1:-}"
  [ -n "$name" ] || name="$(infra_get BACKUP_PREFIX)-import-$(date -u +%Y%m%dT%H%M%SZ).dump"
  printf 's3://%s/%s/%s' "$(infra_get BACKUP_BUCKET)" "$(infra_get BACKUP_PREFIX)" "$name"
}

# Stream deployment $1's dump KEY ($2) to stdout, as that deployment (its own
# account's credentials; as_deployment).
backup_stream_out() {
  infra_env_load
  local prefix
  prefix="$(infra_get BACKUP_PREFIX)"
  [[ "$1" == "$prefix/"*.dump ]] || die "$1 is not one of $DEP's dumps ($prefix/...dump; backup list)"
  aws s3 cp "s3://$(infra_get BACKUP_BUCKET)/$1" - --only-show-errors
}

# backup import - [--as NAME] | --from-deployment prod KEY: a dump into the
# deployment's own dumps, where `backup restore` takes it. From stdin (the
# move's dumps of the old stack; --as keeps a dump's own name), or prod's dump
# KEY into staging (the restore drill): prod's data reaches staging by this
# command alone, and nothing of staging's reaches prod. Read as the source
# deployment, written as this one: the two accounts' credentials never meet in
# one call.
backup_import() {
  local from="" key="" stdin=0 yes=0 name=""
  while [ $# -gt 0 ]; do
    case "$1" in
      -) stdin=1; shift ;;
      --from-deployment) [ $# -ge 3 ] || die "--from-deployment needs a deployment and the dump's KEY"; from="$2"; key="$3"; shift 3 ;;
      --as) [ $# -ge 2 ] || die "--as needs the dump's name"; name="$2"; shift 2 ;;
      --yes) yes=1; shift ;;
      *) die "unknown: backup import $1 (try: - [--as NAME] | --from-deployment prod KEY)" ;;
    esac
  done
  if [ -n "$name" ] && ! [[ "$name" =~ ^novaworld-[A-Za-z0-9._-]+\.dump$ ]]; then
    die "--as takes a dump's name (novaworld-<time>.dump), not '$name'"
  fi
  if [ "$stdin" -eq 1 ] && [ -n "$from" ]; then die "backup import takes - or --from-deployment, not both"; fi
  [ "$stdin" -eq 1 ] || [ -n "$from" ] || die "backup import needs - (a dump on stdin) or --from-deployment prod KEY"
  if [ -n "$from" ]; then
    if [ "$from" != prod ] || [ "$DEP" != staging ]; then
      die "--from-deployment copies prod's dump into staging alone: staging's data never reaches prod, and prod restores its own dumps"
    fi
    if ! [[ "$key" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]*$ ]] || [[ "$key" == *..* ]]; then die "'$key' is no key"; fi
  fi
  [ "$stdin" -eq 0 ] || confirm_prod "write the dump on stdin into prod's dumps" "$yes"
  aws_auth
  infra_env_load
  local target
  target="$(backup_import_target "$name")"
  if [ -n "$from" ]; then
    log "backup import: prod's $key (prod's account) -> $target ($DEP's), streamed through this toolbox"
    as_deployment prod backup_stream_out "$key" \
      | aws s3 cp - "$target" --only-show-errors --expected-size 10737418240 \
      || die "the copy failed; nothing was restored"
  else
    [ ! -t 0 ] || die "backup import - reads the dump from stdin: pipe it in (DEPLOY.md)"
    log "backup import: stdin -> $target"
    aws s3 cp - "$target" --only-show-errors --expected-size 10737418240 \
      || die "the upload failed; nothing was restored"
  fi
  log "imported: ${target#s3://*/}. Restore it with ./deploy/run.sh$DEP_FLAG backup restore ${target#s3://*/} --yes-overwrite-database"
}

cmd_backup() {
  local action="${1:-}"
  shift || true
  case "$action" in
    now)
      aws_auth
      one_off_task '["/backup/backup.sh"]' 1800 || die "the dump failed" ;;
    list)
      aws_auth
      infra_env_load
      aws s3 ls "s3://$(infra_get BACKUP_BUCKET)/$(infra_get BACKUP_PREFIX)/" --recursive --human-readable ;;
    restore) backup_restore "$@" ;;
    import)  backup_import "$@" ;;
    *) die "unknown: backup $action (try: now|list|restore|import)" ;;
  esac
}
