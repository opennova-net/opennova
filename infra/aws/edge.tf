# ---------------------------------------------------------------------------
# Cloudflare: the site's DNS records and edge authentication
# ---------------------------------------------------------------------------

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

# Edge authentication (DEPLOY.md), created only once var.edge_auth_secret is
# set and the web records are proxied: a request-header Transform Rule stamps
# X-OpenNova-Edge: <secret> on every request Cloudflare forwards for the web
# hostnames, so the portal's nginx (EDGE_AUTH_SECRET) can refuse /api/
# requests that reached it any other way, a Cloudflare Worker on someone else's
# zone fetching this origin with a CF-Connecting-IP of its choosing included.
# Unproxied records reach the origin with no Transform Rule, so the rule (and
# the secret beside it) would only refuse every /api/ request. A zone holds one
# entrypoint ruleset per phase: import an existing late-transform ruleset
# before applying, and Terraform then owns all its rules (this one replaces
# them).
resource "cloudflare_ruleset" "edge_auth" {
  count       = local.dns_enabled && var.cloudflare_web_proxied && var.edge_auth_secret != "" ? 1 : 0
  zone_id     = var.cloudflare_zone_id
  name        = "opennova-edge-auth"
  description = "Stamps the edge-authentication header the portal's nginx checks"
  kind        = "zone"
  phase       = "http_request_late_transform"

  rules {
    action      = "rewrite"
    description = "X-OpenNova-Edge on the web hostnames"
    enabled     = true
    expression  = "(http.host in {\"${var.cloudflare_domain}\" \"www.${var.cloudflare_domain}\"})"

    action_parameters {
      headers {
        name      = "X-OpenNova-Edge"
        operation = "set"
        value     = var.edge_auth_secret
      }
    }
  }
}

# A secret with unproxied web records makes no rule (above); the plan says so,
# since the web container given the same secret would then refuse every /api/.
check "edge_auth_needs_proxied_web_records" {
  assert {
    condition     = var.edge_auth_secret == "" || var.cloudflare_web_proxied
    error_message = "edge_auth_secret is set but cloudflare_web_proxied is false: no Transform Rule is made, so keep EDGE_AUTH_SECRET out of the web container's environment (DEPLOY.md, Edge authentication)."
  }
}
