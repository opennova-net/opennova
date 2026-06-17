#!/bin/sh
set -eu

HTPASSWD_FILE="/etc/nginx/conf.d/admin.htpasswd"
COST="${ADMIN_BASIC_AUTH_COST:-10}"

create_htpasswd() {
    user="$1"
    password="$2"
    htpasswd -nbB -C "$COST" "$user" "$password" > "$HTPASSWD_FILE"
}

if [ -n "${ADMIN_BASIC_AUTH_HTPASSWD:-}" ]; then
    printf '%s\n' "$ADMIN_BASIC_AUTH_HTPASSWD" > "$HTPASSWD_FILE"
elif [ -n "${ADMIN_BASIC_AUTH_USER:-}" ] && [ -n "${ADMIN_BASIC_AUTH_PASSWORD:-}" ]; then
    create_htpasswd "$ADMIN_BASIC_AUTH_USER" "$ADMIN_BASIC_AUTH_PASSWORD"
else
    DEFAULT_USER="${ADMIN_BASIC_AUTH_DEFAULT_USER:-admin}"
    DEFAULT_PASSWORD="${ADMIN_BASIC_AUTH_DEFAULT_PASSWORD:-admin}"
    echo "WARNING: ADMIN_BASIC_AUTH_USER/ADMIN_BASIC_AUTH_PASSWORD not set. Falling back to ${DEFAULT_USER}:${DEFAULT_PASSWORD}." >&2
    create_htpasswd "$DEFAULT_USER" "$DEFAULT_PASSWORD"
fi

chmod 640 "$HTPASSWD_FILE"
chown nginx:nginx "$HTPASSWD_FILE"

exec "$@"
