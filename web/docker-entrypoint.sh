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

# Origin TLS (DEPLOY.md): with ORIGIN_TLS_CERT and ORIGIN_TLS_KEY set (each a
# PEM, base64 on one line, as the host's env file carries them: the Cloudflare
# Origin CA certificate infra/aws/edge.tf makes), the site listens on 443
# with them, and Cloudflare verifies this origin ("Full (strict)"). The two are
# written to ORIGIN_TLS_DIR (a tmpfs in the prod overlay), the key owner-only.
# Neither set: plain 80, as a local run of the image wants. One alone stops
# the container.
LISTEN_FILE="/etc/nginx/listen.conf"
TLS_DIR="${ORIGIN_TLS_DIR:-/run/tls}"

if [ -n "${ORIGIN_TLS_CERT:-}" ] && [ -n "${ORIGIN_TLS_KEY:-}" ]; then
    mkdir -p "$TLS_DIR"
    chmod 700 "$TLS_DIR"
    old_umask="$(umask)"
    umask 077
    if ! printf '%s' "$ORIGIN_TLS_CERT" | base64 -d > "$TLS_DIR/origin.crt" 2>/dev/null \
        || ! grep -q 'BEGIN CERTIFICATE' "$TLS_DIR/origin.crt"; then
        echo "ERROR: ORIGIN_TLS_CERT is no base64 PEM certificate" >&2
        exit 1
    fi
    if ! printf '%s' "$ORIGIN_TLS_KEY" | base64 -d > "$TLS_DIR/origin.key" 2>/dev/null \
        || ! grep -q 'PRIVATE KEY' "$TLS_DIR/origin.key"; then
        echo "ERROR: ORIGIN_TLS_KEY is no base64 PEM private key" >&2
        exit 1
    fi
    umask "$old_umask"
    {
        printf 'listen 443 ssl;\n'
        printf 'http2 on;\n'
        printf 'ssl_certificate %s/origin.crt;\n' "$TLS_DIR"
        printf 'ssl_certificate_key %s/origin.key;\n' "$TLS_DIR"
        printf 'ssl_protocols TLSv1.2 TLSv1.3;\n'
        printf 'ssl_session_cache shared:origin:1m;\n'
    } > "$LISTEN_FILE"
    echo "origin TLS: ON (:443, the Origin CA certificate)" >&2
elif [ -n "${ORIGIN_TLS_CERT:-}" ] || [ -n "${ORIGIN_TLS_KEY:-}" ]; then
    echo "ERROR: ORIGIN_TLS_CERT and ORIGIN_TLS_KEY go together" >&2
    exit 1
else
    printf 'listen 80;\n' > "$LISTEN_FILE"
    echo "origin TLS: OFF (:80; ORIGIN_TLS_CERT unset)" >&2
fi

# Edge authentication (DEPLOY.md): with EDGE_AUTH_SECRET set, this nginx forwards
# /api/ only for requests carrying X-OpenNova-Edge: <secret>, which a Cloudflare
# Transform Rule on this zone adds, so a request that reached this origin's port
# 80 any other way (a Cloudflare Worker on another zone included, with whatever
# CF-Connecting-IP it chose) is refused here. The server's own port is closed to
# /api/ by ONNET_API_PROXY_ONLY, not by this. EDGE_AUTH_SECRET_PREVIOUS, set only
# during a rotation, is a second value accepted beside it, so the rule can move
# to the new secret while both pass. nginx compares map strings without regard
# to case, so a secret's case adds nothing: use lowercase hex (openssl rand -hex
# 32). Unset, the image's edge-auth.conf keeps it off.
EDGE_AUTH_FILE="/etc/nginx/edge-auth.conf"

# A secret: letters and digits, at least 32 of them; else the container stops.
check_edge_secret() {
    name="$1"
    value="$2"
    case "$value" in
        *[!A-Za-z0-9]*)
            echo "ERROR: $name may hold letters and digits only" >&2
            exit 1
            ;;
    esac
    if [ "${#value}" -lt 32 ]; then
        echo "ERROR: $name must be at least 32 characters" >&2
        exit 1
    fi
}

if [ -n "${EDGE_AUTH_SECRET:-}" ]; then
    check_edge_secret EDGE_AUTH_SECRET "$EDGE_AUTH_SECRET"
    secrets="$EDGE_AUTH_SECRET"
    longest=${#EDGE_AUTH_SECRET}
    if [ -n "${EDGE_AUTH_SECRET_PREVIOUS:-}" ]; then
        check_edge_secret EDGE_AUTH_SECRET_PREVIOUS "$EDGE_AUTH_SECRET_PREVIOUS"
        # The map takes each value once, case aside.
        if [ "$(printf '%s' "$EDGE_AUTH_SECRET" | tr '[:upper:]' '[:lower:]')" = \
             "$(printf '%s' "$EDGE_AUTH_SECRET_PREVIOUS" | tr '[:upper:]' '[:lower:]')" ]; then
            echo "ERROR: EDGE_AUTH_SECRET_PREVIOUS is the same secret as EDGE_AUTH_SECRET" >&2
            exit 1
        fi
        secrets="$secrets $EDGE_AUTH_SECRET_PREVIOUS"
        if [ "${#EDGE_AUTH_SECRET_PREVIOUS}" -gt "$longest" ]; then
            longest=${#EDGE_AUTH_SECRET_PREVIOUS}
        fi
    fi
    # nginx keeps each map key whole in one hash bucket: an entry is its value
    # pointer (8 bytes) and the key's length (2) and bytes, rounded up to 8, and
    # the bucket ends with an 8-byte terminator, so a key of N characters needs
    # ((N + 9) / 8) * 8 + 16 bytes. The default bucket (64 bytes, a cache line)
    # holds no key past 46 characters: a 64-digit `openssl rand -hex 32` secret
    # stops nginx ("could not build map_hash"). The bucket is the smallest
    # power of two from 64 that holds the longest secret, set before the map,
    # which reads it.
    need=$(( ((longest + 9) / 8) * 8 + 16 ))
    bucket=64
    while [ "$bucket" -lt "$need" ]; do
        bucket=$((bucket * 2))
    done
    {
        printf 'map_hash_bucket_size %s;\n' "$bucket"
        # shellcheck disable=SC2016  # nginx's variables, written as they are
        printf 'map $http_x_opennova_edge $opennova_edge_refused {\n    default 1;\n'
        for secret in $secrets; do
            printf '    "%s" 0;\n' "$secret"
        done
        printf '}\n'
    } > "$EDGE_AUTH_FILE"
    chmod 600 "$EDGE_AUTH_FILE"
    if [ -n "${EDGE_AUTH_SECRET_PREVIOUS:-}" ]; then
        echo "edge authentication: ON (/api/ requires X-OpenNova-Edge; the previous secret is accepted too, for the rotation)" >&2
    else
        echo "edge authentication: ON (/api/ requires X-OpenNova-Edge)" >&2
    fi
elif [ -n "${EDGE_AUTH_SECRET_PREVIOUS:-}" ]; then
    echo "ERROR: EDGE_AUTH_SECRET_PREVIOUS is set without EDGE_AUTH_SECRET" >&2
    exit 1
else
    echo "edge authentication: OFF (EDGE_AUTH_SECRET unset)" >&2
fi

exec "$@"
