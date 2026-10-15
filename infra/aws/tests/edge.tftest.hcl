# Cloudflare in front of the portal (edge.tf), planned against mock providers
# whose values reach the plan: offline, no credentials, nothing reaches AWS or
# Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Proxied records: an Origin CA certificate (ECDSA) for the domain and its
# first-level names, its certificate and key handed to the host through SSM
# SecureStrings, the zone on Full (strict) with https forced, edge
# authentication stamping every deployment's web hostname, and Stripe's
# webhook (POST /api/billing/webhook alone) skipping the WAF. Unproxied or
# without Cloudflare: none of it. Prod without an edge secret says so.

mock_provider "aws" {
  override_during = plan

  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a", "us-east-1b", "us-east-1c"] }
  }
  mock_data "aws_ami" {
    defaults = { id = "ami-0123456789abcdef0", root_device_name = "/dev/sda1" }
  }
  mock_data "aws_ebs_snapshot" {
    defaults = { volume_size = 2 }
  }
  mock_data "aws_caller_identity" {
    defaults = { account_id = "123456789012" }
  }
  mock_data "aws_partition" {
    defaults = { partition = "aws" }
  }
  mock_data "aws_iam_policy_document" {
    defaults = { json = "{\"Version\":\"2012-10-17\",\"Statement\":[]}" }
  }
  mock_data "aws_eip" {
    defaults = { id = "eipalloc-0123456789abcdef0", public_ip = "54.162.79.147" }
  }
  mock_resource "aws_eip" {
    defaults = { allocation_id = "eipalloc-0fedcba9876543210", public_ip = "3.3.3.3" }
  }
  # Well-formed ARNs and ids for what the configuration hands to an ARN or id
  # argument (the provider's own validation reads them).
  mock_resource "aws_iam_instance_profile" {
    defaults = { arn = "arn:aws:iam::123456789012:instance-profile/mock" }
  }
  mock_resource "aws_iam_policy" {
    defaults = { arn = "arn:aws:iam::123456789012:policy/mock" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::123456789012:role/mock" }
  }
  mock_resource "aws_cloudwatch_log_group" {
    defaults = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/mock" }
  }
  mock_resource "aws_ssm_document" {
    defaults = { arn = "arn:aws:ssm:us-east-1:123456789012:document/opennova-server-converge" }
  }
  mock_resource "aws_sns_topic" {
    defaults = { arn = "arn:aws:sns:us-east-1:123456789012:opennova-server-alerts" }
  }
  mock_resource "aws_db_instance" {
    defaults = { address = "opennova-server-db.abcdefghijkl.us-east-1.rds.amazonaws.com", port = 5432, id = "db-MOCK" }
  }
  mock_resource "aws_launch_template" {
    defaults = { arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0123456789abcdef0", id = "lt-0123456789abcdef0" }
  }
  mock_resource "aws_instance" {
    defaults = { id = "i-0123456789abcdef0" }
  }
}

mock_provider "cloudflare" {
  override_during = plan

  mock_data "cloudflare_ip_ranges" {
    defaults = {
      ipv4_cidr_blocks = ["173.245.48.0/20", "103.21.244.0/22"]
      ipv6_cidr_blocks = ["2400:cb00::/32"]
    }
  }
  mock_resource "cloudflare_origin_ca_certificate" {
    defaults = { certificate = "-----BEGIN CERTIFICATE-----\nMOCK\n-----END CERTIFICATE-----\n" }
  }
}

# The ids the assertions compare, known at plan.
override_resource {
  target          = aws_subnet.public["a"]
  override_during = plan
  values          = { id = "subnet-0public0a" }
}
override_resource {
  target          = aws_subnet.public["b"]
  override_during = plan
  values          = { id = "subnet-0public0b" }
}
override_resource {
  target          = aws_subnet.private["a"]
  override_during = plan
  values          = { id = "subnet-0private0a" }
}
override_resource {
  target          = aws_subnet.private["b"]
  override_during = plan
  values          = { id = "subnet-0private0b" }
}
override_resource {
  target          = aws_security_group.service
  override_during = plan
  values          = { id = "sg-0service" }
}
override_resource {
  target          = aws_security_group.db
  override_during = plan
  values          = { id = "sg-0db" }
}
override_resource {
  target          = aws_network_interface.service
  override_during = plan
  values          = { id = "eni-0service" }
}

# The tls provider is the real one: it works offline, and its key and request
# stay unknown at plan (the Cloudflare provider parses a request it is given,
# so a mock one would not do).

variables {
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cloudflare_zone_id        = "0123456789abcdef0123456789abcdef"
  cloudflare_domain         = "example.com"
  edge_auth_secret          = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}

run "edge" {
  command = plan

  assert {
    condition     = tls_private_key.origin[0].algorithm == "ECDSA" && tls_private_key.origin[0].ecdsa_curve == "P256"
    error_message = "the origin's key: ECDSA P-256"
  }
  assert {
    condition = (
      toset(cloudflare_origin_ca_certificate.origin[0].hostnames) == toset(["example.com", "*.example.com"]) &&
      cloudflare_origin_ca_certificate.origin[0].request_type == "origin-ecc"
    )
    error_message = "an Origin CA certificate for the domain and its first-level names"
  }
  assert {
    condition = (
      aws_ssm_parameter.origin_tls_cert[0].name == "/opennova/prod/tls/ORIGIN_TLS_CERT" && aws_ssm_parameter.origin_tls_cert[0].type == "SecureString" &&
      aws_ssm_parameter.origin_tls_key[0].name == "/opennova/prod/tls/ORIGIN_TLS_KEY" && aws_ssm_parameter.origin_tls_key[0].type == "SecureString" &&
      base64decode(aws_ssm_parameter.origin_tls_cert[0].value) == "-----BEGIN CERTIFICATE-----\nMOCK\n-----END CERTIFICATE-----\n"
    )
    error_message = "the certificate and key reach the host as base64 SecureStrings under /opennova/<deployment>/tls/"
  }
  assert {
    condition = (
      cloudflare_zone_settings_override.zone[0].settings[0].ssl == "strict" &&
      cloudflare_zone_settings_override.zone[0].settings[0].always_use_https == "on"
    )
    error_message = "the zone verifies the origin (Full strict) and forces https"
  }
  assert {
    condition = (
      cloudflare_ruleset.edge_auth[0].rules[0].expression == "(http.host in {\"example.com\" \"www.example.com\" \"staging.example.com\"})" &&
      cloudflare_ruleset.edge_auth[0].rules[0].action_parameters[0].headers[0].name == "X-OpenNova-Edge"
    )
    error_message = "edge authentication stamps every deployment's web hostname"
  }
  assert {
    condition = (
      strcontains(cloudflare_ruleset.webhook_skip[0].rules[0].expression, "http.request.uri.path eq \"/api/billing/webhook\"") &&
      strcontains(cloudflare_ruleset.webhook_skip[0].rules[0].expression, "http.request.method eq \"POST\"") &&
      cloudflare_ruleset.webhook_skip[0].rules[0].action == "skip" &&
      cloudflare_ruleset.webhook_skip[0].phase == "http_request_firewall_custom"
    )
    error_message = "Stripe's webhook, and it alone, skips the WAF"
  }
}

run "no_secret_says_so" {
  command = plan

  variables {
    edge_auth_secret = ""
  }

  expect_failures = [check.edge_auth_is_on]

  assert {
    condition     = length(cloudflare_ruleset.edge_auth) == 0
    error_message = "no secret, no rule"
  }
}

run "unproxied" {
  command = plan

  variables {
    cloudflare_web_proxied = false
  }

  assert {
    condition = (
      length(tls_private_key.origin) == 0 && length(cloudflare_origin_ca_certificate.origin) == 0 &&
      length(cloudflare_zone_settings_override.zone) == 0 && length(cloudflare_ruleset.edge_auth) == 0 &&
      length(cloudflare_ruleset.webhook_skip) == 0
    )
    error_message = "unproxied records: no origin certificate and no zone rule"
  }
}

run "no_cloudflare" {
  command = plan

  variables {
    cloudflare_zone_id = ""
    cloudflare_domain  = ""
  }

  assert {
    condition     = length(aws_ssm_parameter.origin_tls_cert) == 0 && length(cloudflare_ruleset.webhook_skip) == 0
    error_message = "without Cloudflare: none of it"
  }
}
