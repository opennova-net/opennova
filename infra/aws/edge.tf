# ---------------------------------------------------------------------------
# Cloudflare: the site's DNS records, its origin's TLS, edge authentication and
# the webhook's way past the WAF (DEPLOY.md, section 3)
# ---------------------------------------------------------------------------
#
# Cloudflare proxies the site's hostnames and talks to the origin (the portal's
# nginx, port 443, which admits Cloudflare's ranges alone) over TLS it verifies
# ("Full (strict)"): the origin presents a Cloudflare Origin CA certificate
# made here, whose key Terraform keeps in its state and hands the host through
# SSM (SecureString, /opennova/<deployment>/tls/*; the host's converge puts both
# in the web container's environment, and its entrypoint writes them to a
# tmpfs). Site passwords never cross the internet in clear. The retail HTTP
# port (8080) is no part of this: stock clients speak plain HTTP to it.
#
# The zone's settings and its entrypoint rulesets (one per phase per zone) are
# the zone's, so prod (the default workspace) owns them, naming every
# deployment's web hostname (var.edge_other_deployments).

# The site's records, all at the service host's Elastic IP: prod's @, www and
# game; another deployment's <deployment> and game-<deployment> (locals.tf).
# Proxied by default so the site gets Cloudflare TLS.
resource "cloudflare_record" "web_root" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.manage_shared ? "@" : local.deployment
  content = local.service_public_ip
  type    = "A"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web"
}

resource "cloudflare_record" "web_www" {
  count   = local.dns_enabled && local.manage_shared ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = "www"
  content = local.service_public_ip
  type    = "A"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web (www)"
}

# The web build (ADR 0049): the portal's nginx routes game.<domain> to the game
# container. Proxied like the root, so it gets the same Cloudflare TLS.
resource "cloudflare_record" "game" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.manage_shared ? "game" : "game-${local.deployment}"
  content = local.service_public_ip
  type    = "A"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web build"
}

locals {
  # Cloudflare in front of the origin: proxied records on a configured zone.
  edge_proxied = local.dns_enabled && var.cloudflare_web_proxied

  # Every deployment's web hostnames: prod's domain and www, and each other
  # deployment's <name>.<domain>. Edge authentication and the webhook rule
  # cover them all (one ruleset per phase per zone).
  edge_web_hosts  = concat([var.cloudflare_domain, "www.${var.cloudflare_domain}"], [for d in var.edge_other_deployments : "${d}.${var.cloudflare_domain}"])
  edge_hosts_expr = "http.host in {${join(" ", [for h in local.edge_web_hosts : "\"${h}\""])}}"
}

# ---- the origin's certificate --------------------------------------------------

resource "tls_private_key" "origin" {
  count       = local.edge_proxied ? 1 : 0
  algorithm   = "ECDSA"
  ecdsa_curve = "P256"
}

resource "tls_cert_request" "origin" {
  count           = local.edge_proxied ? 1 : 0
  private_key_pem = tls_private_key.origin[0].private_key_pem

  subject {
    common_name  = var.cloudflare_domain
    organization = "OpenNova"
  }
}

# A Cloudflare Origin CA certificate (trusted by Cloudflare alone, which is the
# point: only Cloudflare talks to this origin) for the domain and its
# first-level names, 15 years. The API token needs Zone / SSL and Certificates /
# Edit. Renewing is a -replace of it, then an app deploy.
resource "cloudflare_origin_ca_certificate" "origin" {
  count              = local.edge_proxied ? 1 : 0
  csr                = tls_cert_request.origin[0].cert_request_pem
  hostnames          = [var.cloudflare_domain, "*.${var.cloudflare_domain}"]
  request_type       = "origin-ecc"
  requested_validity = 5475
}

# The certificate and its key for the host, base64 (one line each, as the env
# file carries them): converge reads /opennova/<deployment>/tls/* beside the
# secrets.
resource "aws_ssm_parameter" "origin_tls_cert" {
  count       = local.edge_proxied ? 1 : 0
  name        = "${local.ssm_prefix}/tls/ORIGIN_TLS_CERT"
  description = "The portal's Cloudflare Origin CA certificate (base64 PEM; infra/aws/edge.tf)"
  type        = "SecureString"
  tier        = "Standard"
  value       = base64encode(cloudflare_origin_ca_certificate.origin[0].certificate)

  tags = var.tags
}

resource "aws_ssm_parameter" "origin_tls_key" {
  count       = local.edge_proxied ? 1 : 0
  name        = "${local.ssm_prefix}/tls/ORIGIN_TLS_KEY"
  description = "The portal's origin certificate's private key (base64 PEM; infra/aws/edge.tf)"
  type        = "SecureString"
  tier        = "Standard"
  value       = base64encode(tls_private_key.origin[0].private_key_pem)

  tags = var.tags
}

# ---- the zone's settings (prod's) ------------------------------------------------

# Cloudflare verifies the origin's certificate ("Full (strict)"), and every
# visitor is sent to https. Apply this together with an app deploy whose nginx
# serves 443: until both are in, the site answers 52x.
resource "cloudflare_zone_settings_override" "zone" {
  count   = local.manage_shared && local.edge_proxied ? 1 : 0
  zone_id = var.cloudflare_zone_id

  settings {
    ssl              = "strict"
    always_use_https = "on"
    min_tls_version  = "1.2"
  }
}

# ---- edge authentication (prod's) ----------------------------------------------

# A request-header Transform Rule stamps X-OpenNova-Edge: <secret> on every
# request Cloudflare forwards for the web hostnames, so the portal's nginx
# (EDGE_AUTH_SECRET, the same value, from the vault's app-prod item for every
# deployment) refuses /api/ requests that reached it any other way: a
# Cloudflare Worker on someone else's zone fetching this origin with a
# CF-Connecting-IP of its choosing included. A zone holds one entrypoint
# ruleset per phase: import an existing late-transform ruleset before
# applying, and Terraform then owns all its rules (this one replaces them).
resource "cloudflare_ruleset" "edge_auth" {
  count       = local.manage_shared && local.edge_proxied && var.edge_auth_secret != "" ? 1 : 0
  zone_id     = var.cloudflare_zone_id
  name        = "opennova-edge-auth"
  description = "Stamps the edge-authentication header the portal's nginx checks"
  kind        = "zone"
  phase       = "http_request_late_transform"

  rules {
    action      = "rewrite"
    description = "X-OpenNova-Edge on the web hostnames"
    enabled     = true
    expression  = "(${local.edge_hosts_expr})"

    action_parameters {
      headers {
        name      = "X-OpenNova-Edge"
        operation = "set"
        value     = var.edge_auth_secret
      }
    }
  }
}

# Prod's plan names a missing secret: the web container requires one.
check "edge_auth_is_on" {
  assert {
    condition     = !(local.manage_shared && local.edge_proxied) || var.edge_auth_secret != ""
    error_message = "no edge_auth_secret: no Transform Rule is made, and the web container (which requires EDGE_AUTH_SECRET) refuses every /api/ request. Run ./deploy/run.sh secrets init (it makes app-prod/edge_auth_secret)."
  }
}

# ---- Stripe's webhook past the WAF (prod's) ----------------------------------------

# Stripe posts billing events to /api/billing/webhook from its own servers, a
# client Cloudflare's security features may challenge (and a challenged
# webhook never arrives). This custom rule skips the rest of the custom rules,
# the managed and rate-limiting rules and the skippable legacy products for that
# one request; the server verifies Stripe's signature itself. Bot Fight Mode
# (the free plan's) cannot be skipped by any rule: keep it off, and check
# delivery with Stripe's "Send test webhook" (DEPLOY.md). A zone holds one
# custom-rules entrypoint ruleset: import an existing one before applying.
resource "cloudflare_ruleset" "webhook_skip" {
  count       = local.manage_shared && local.edge_proxied ? 1 : 0
  zone_id     = var.cloudflare_zone_id
  name        = "opennova-custom-rules"
  description = "Lets Stripe's webhook through to the portal unchallenged"
  kind        = "zone"
  phase       = "http_request_firewall_custom"

  rules {
    action      = "skip"
    description = "Stripe webhook: POST /api/billing/webhook"
    enabled     = true
    expression  = "(${local.edge_hosts_expr} and http.request.method eq \"POST\" and http.request.uri.path eq \"/api/billing/webhook\")"

    action_parameters {
      ruleset  = "current"
      phases   = var.cloudflare_webhook_skip_phases
      products = var.cloudflare_webhook_skip_products
    }

    logging {
      enabled = true
    }
  }
}
