# ---------------------------------------------------------------------------
# The zone's TLS, edge authentication and the webhook's way past the WAF
# (DEPLOY.md, section 3)
# ---------------------------------------------------------------------------
#
# Cloudflare proxies every deployment's site and talks to its origin (that
# deployment's portal nginx, behind its load balancer: infra/aws) over TLS it
# verifies, "Full (strict)", against the origin certificate infra/aws makes for
# that deployment alone.

locals {
  # Each deployment's web hostnames, as infra/aws names its site.
  deployment_hosts = {
    prod    = [var.cloudflare_domain, "www.${var.cloudflare_domain}"]
    staging = ["staging.${var.cloudflare_domain}"]
  }
  edge_secrets = {
    prod    = var.edge_auth_secret_prod
    staging = var.edge_auth_secret_staging
  }

  host_expr = { for d, hosts in local.deployment_hosts : d => "http.host in {${join(" ", [for h in hosts : "\"${h}\""])}}" }
  # Every deployment's hostnames, for what applies to them all.
  all_hosts_expr = "http.host in {${join(" ", [for h in flatten([for d in ["prod", "staging"] : local.deployment_hosts[d]]) : "\"${h}\""])}}"
}

# ---- the zone's settings --------------------------------------------------------

# Cloudflare verifies each origin's certificate ("Full (strict)"), and every
# visitor is sent to https. A deployment's site answers 52x until its app
# deploy's nginx serves 443 with its certificate.
resource "cloudflare_zone_settings_override" "zone" {
  zone_id = var.cloudflare_zone_id

  settings {
    ssl              = "strict"
    always_use_https = "on"
    min_tls_version  = "1.2"
  }
}

# ---- edge authentication ---------------------------------------------------------

# A request-header Transform Rule per deployment stamps X-OpenNova-Edge with
# that deployment's secret on the requests for its hostnames alone, so its
# portal's nginx (EDGE_AUTH_SECRET, from the vault's app-<deployment> item)
# refuses /api/ requests that reached it any other way: a Cloudflare Worker on
# someone else's zone fetching the origin with a CF-Connecting-IP of its
# choosing among them. A zone holds one entrypoint ruleset per phase: import an
# existing late-transform ruleset before applying, and this stack then owns all
# its rules (these replace them).
resource "cloudflare_ruleset" "edge_auth" {
  zone_id     = var.cloudflare_zone_id
  name        = "opennova-edge-auth"
  description = "Stamps each deployment's edge-authentication header the portal's nginx checks"
  kind        = "zone"
  phase       = "http_request_late_transform"

  dynamic "rules" {
    for_each = ["prod", "staging"]
    content {
      action      = "rewrite"
      description = "X-OpenNova-Edge on ${rules.value}'s web hostnames"
      enabled     = true
      expression  = "(${local.host_expr[rules.value]})"

      action_parameters {
        headers {
          name      = "X-OpenNova-Edge"
          operation = "set"
          value     = local.edge_secrets[rules.value]
        }
      }
    }
  }

  lifecycle {
    # One deployment's secret opening another's origin is what the split is for.
    precondition {
      condition     = var.edge_auth_secret_prod != var.edge_auth_secret_staging
      error_message = "prod and staging hold the same edge_auth_secret: staging's would open prod's origin. Give app-staging a secret of its own (openssl rand -hex 32; DEPLOY.md, \"Edge authentication\")."
    }
  }
}

# ---- Stripe's webhook past the WAF -------------------------------------------------

# Stripe posts billing events to /api/billing/webhook from its own servers, a
# client Cloudflare's security features may challenge (and a challenged
# webhook never arrives). This custom rule skips the rest of the custom rules,
# the managed and rate-limiting rules and the skippable legacy products for that
# one request, on every deployment's site (live mode's endpoint is prod's, test
# mode's staging's); each server verifies Stripe's signature itself. Bot Fight
# Mode (the free plan's) cannot be skipped by any rule: keep it off, and check
# delivery with Stripe's "Send test webhook" (DEPLOY.md). A zone holds one
# custom-rules entrypoint ruleset: import an existing one before applying.
resource "cloudflare_ruleset" "webhook_skip" {
  zone_id     = var.cloudflare_zone_id
  name        = "opennova-custom-rules"
  description = "Lets Stripe's webhook through to the portal unchallenged"
  kind        = "zone"
  phase       = "http_request_firewall_custom"

  rules {
    action      = "skip"
    description = "Stripe webhook: POST /api/billing/webhook"
    enabled     = true
    expression  = "(${local.all_hosts_expr} and http.request.method eq \"POST\" and http.request.uri.path eq \"/api/billing/webhook\")"

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
