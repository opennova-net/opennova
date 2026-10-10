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

# Edge authentication (DEPLOY.md): with EDGE_AUTH_SECRET set, /api/ answers only
# requests carrying X-OpenNova-Edge: <secret>, which a Cloudflare Transform Rule
# on this zone adds, so a request that reached this origin any other way (a
# Cloudflare Worker on another zone included, with whatever CF-Connecting-IP it
# chose) never reaches the NovaWorld server. Unset, the image's edge-auth.conf
# keeps it off.
EDGE_AUTH_FILE="/etc/nginx/edge-auth.conf"
if [ -n "${EDGE_AUTH_SECRET:-}" ]; then
    case "$EDGE_AUTH_SECRET" in
        *[!A-Za-z0-9]*)
            echo "ERROR: EDGE_AUTH_SECRET may hold letters and digits only" >&2
            exit 1
            ;;
    esac
    if [ "${#EDGE_AUTH_SECRET}" -lt 32 ]; then
        echo "ERROR: EDGE_AUTH_SECRET must be at least 32 characters" >&2
        exit 1
    fi
    printf 'map $http_x_opennova_edge $opennova_edge_refused {\n    default 1;\n    "%s" 0;\n}\n' \
        "$EDGE_AUTH_SECRET" > "$EDGE_AUTH_FILE"
    chmod 600 "$EDGE_AUTH_FILE"
    echo "edge authentication: ON (/api/ requires X-OpenNova-Edge)" >&2
else
    echo "edge authentication: OFF (EDGE_AUTH_SECRET unset)" >&2
fi

exec "$@"
