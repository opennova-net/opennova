# The zone's shared settings (edge.tf, email.tf), planned against a mock
# provider: offline, no token, nothing reaches Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/zone hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/zone hashicorp/terraform:<the toolbox's> test
# The zone on Full (strict) with https forced; edge authentication a rule per
# deployment, each stamping its own secret on its own hostnames alone (prod's
# never on staging's, staging's never on prod's), and one secret for both
# refused; Stripe's webhook skipping the WAF on every deployment's site; Email
# Routing to the destination (support@ and each deployment account's root
# address, aws-prod@ and aws-staging@), off with a placeholder, refusing a loop.

mock_provider "cloudflare" {
  override_during = plan

  mock_data "cloudflare_zone" {
    defaults = { account_id = "fedcba9876543210fedcba9876543210" }
  }
}

variables {
  cloudflare_zone_id       = "0123456789abcdef0123456789abcdef"
  cloudflare_domain        = "example.com"
  edge_auth_secret_prod    = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  edge_auth_secret_staging = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
  email_forward_to         = "maintainer@mail.example.org"
}

run "settings" {
  command = plan

  assert {
    condition = (
      cloudflare_zone_settings_override.zone.settings[0].ssl == "strict" &&
      cloudflare_zone_settings_override.zone.settings[0].always_use_https == "on" &&
      cloudflare_zone_settings_override.zone.settings[0].min_tls_version == "1.2"
    )
    error_message = "the zone verifies each origin (Full strict), forces https, TLS 1.2 at least"
  }
}

run "edge_auth_per_deployment" {
  command = plan

  assert {
    condition     = length(cloudflare_ruleset.edge_auth.rules) == 2 && cloudflare_ruleset.edge_auth.phase == "http_request_late_transform"
    error_message = "edge authentication is one late-transform rule per deployment"
  }
  assert {
    condition = (
      cloudflare_ruleset.edge_auth.rules[0].expression == "(http.host in {\"example.com\" \"www.example.com\"})" &&
      cloudflare_ruleset.edge_auth.rules[0].action_parameters[0].headers[0].name == "X-OpenNova-Edge" &&
      cloudflare_ruleset.edge_auth.rules[0].action_parameters[0].headers[0].value == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    )
    error_message = "prod's rule stamps prod's secret on the domain and www alone"
  }
  assert {
    condition = (
      cloudflare_ruleset.edge_auth.rules[1].expression == "(http.host in {\"staging.example.com\"})" &&
      cloudflare_ruleset.edge_auth.rules[1].action_parameters[0].headers[0].name == "X-OpenNova-Edge" &&
      cloudflare_ruleset.edge_auth.rules[1].action_parameters[0].headers[0].value == "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
    )
    error_message = "staging's rule stamps staging's secret on staging.<domain> alone"
  }
  assert {
    condition = (
      !strcontains(cloudflare_ruleset.edge_auth.rules[0].expression, "staging.") &&
      !strcontains(cloudflare_ruleset.edge_auth.rules[1].expression, "\"example.com\"") &&
      !strcontains(cloudflare_ruleset.edge_auth.rules[1].expression, "www.")
    )
    error_message = "no deployment's secret reaches another's hostnames"
  }
  assert {
    condition     = output.deployment_hosts == { prod = ["example.com", "www.example.com"], staging = ["staging.example.com"] }
    error_message = "the hostnames are infra/aws's sites: prod the domain and www, staging staging.<domain>"
  }
}

run "one_secret_for_both_refused" {
  command = plan

  variables {
    edge_auth_secret_staging = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  }

  expect_failures = [cloudflare_ruleset.edge_auth]
}

run "short_secret_refused" {
  command = plan

  variables {
    edge_auth_secret_staging = "REPLACE_ME"
  }

  expect_failures = [var.edge_auth_secret_staging]
}

run "webhook_skip" {
  command = plan

  assert {
    condition = (
      cloudflare_ruleset.webhook_skip.phase == "http_request_firewall_custom" &&
      cloudflare_ruleset.webhook_skip.rules[0].action == "skip" &&
      strcontains(cloudflare_ruleset.webhook_skip.rules[0].expression, "http.host in {\"example.com\" \"www.example.com\" \"staging.example.com\"}") &&
      strcontains(cloudflare_ruleset.webhook_skip.rules[0].expression, "http.request.method eq \"POST\"") &&
      strcontains(cloudflare_ruleset.webhook_skip.rules[0].expression, "http.request.uri.path eq \"/api/billing/webhook\"")
    )
    error_message = "Stripe's webhook, and it alone, skips the WAF on every deployment's site"
  }
}

run "email" {
  command = plan

  assert {
    condition     = cloudflare_email_routing_settings.zone[0].enabled && cloudflare_email_routing_settings.zone[0].zone_id == "0123456789abcdef0123456789abcdef"
    error_message = "Email Routing is on for the zone"
  }
  assert {
    condition = (
      cloudflare_email_routing_address.forward_to[0].email == "maintainer@mail.example.org" &&
      cloudflare_email_routing_address.forward_to[0].account_id == "fedcba9876543210fedcba9876543210"
    )
    error_message = "the destination is the vault's address, in the zone's account"
  }
  assert {
    condition = (
      toset(keys(cloudflare_email_routing_rule.route)) == toset(["support", "aws-prod", "aws-staging"]) &&
      one([for m in cloudflare_email_routing_rule.route["support"].matcher : m.type == "literal" && m.field == "to" && m.value == "support@example.com"]) &&
      one([for m in cloudflare_email_routing_rule.route["aws-prod"].matcher : m.type == "literal" && m.field == "to" && m.value == "aws-prod@example.com"]) &&
      one([for m in cloudflare_email_routing_rule.route["aws-staging"].matcher : m.type == "literal" && m.field == "to" && m.value == "aws-staging@example.com"]) &&
      alltrue([for r in cloudflare_email_routing_rule.route : one([for a in r.action : a.type == "forward" && toset(a.value) == toset(["maintainer@mail.example.org"])])])
    )
    error_message = "support@<domain> and the deployment accounts' root addresses (aws-prod@, aws-staging@) forward to the destination, and no other"
  }
  assert {
    condition     = toset(output.email_routing) == toset(["support@example.com", "aws-prod@example.com", "aws-staging@example.com"])
    error_message = "the forwarded addresses are support@, aws-prod@ and aws-staging@"
  }
}

run "email_placeholder_off" {
  command = plan

  variables {
    email_forward_to = "REPLACE_ME"
  }

  assert {
    condition = (
      length(cloudflare_email_routing_settings.zone) == 0 && length(cloudflare_email_routing_address.forward_to) == 0 &&
      length(cloudflare_email_routing_rule.route) == 0 && length(data.cloudflare_zone.email) == 0 &&
      length(output.email_routing) == 0
    )
    error_message = "a placeholder destination keeps Email Routing off"
  }
}

run "email_loop_refused" {
  command = plan

  variables {
    email_forward_to = "someone@example.com"
  }

  expect_failures = [cloudflare_email_routing_address.forward_to]
}
