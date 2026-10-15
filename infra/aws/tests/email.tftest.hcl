# Cloudflare Email Routing (email.tf), planned against mock providers whose
# values reach the plan: offline, no credentials, nothing reaches AWS or
# Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# With a destination, prod turns Email Routing on for the zone, adds the
# destination address to the zone's account, and forwards each email_routes
# local part (support by default) there by a literal match on the recipient.
# A placeholder or empty destination, or no Cloudflare, makes none of it; a
# destination at the domain itself (a loop) or a malformed one is refused.
# (tests/staging: off prod, none of it.)

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
  mock_data "cloudflare_zone" {
    defaults = { account_id = "fedcba9876543210fedcba9876543210", name = "example.com" }
  }
  mock_resource "cloudflare_origin_ca_certificate" {
    defaults = { certificate = "-----BEGIN CERTIFICATE-----\nMOCK\n-----END CERTIFICATE-----\n" }
  }
}

# The ids the configuration hands on, known at plan.
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

variables {
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cloudflare_zone_id        = "0123456789abcdef0123456789abcdef"
  cloudflare_domain         = "example.com"
  edge_auth_secret          = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  email_forward_to          = "maintainer@mail.example.org"
}

run "email" {
  command = plan

  assert {
    condition     = cloudflare_email_routing_settings.zone[0].enabled && cloudflare_email_routing_settings.zone[0].zone_id == "0123456789abcdef0123456789abcdef"
    error_message = "Email Routing is on for the zone (Cloudflare adds its MX and SPF records)"
  }
  assert {
    condition = (
      cloudflare_email_routing_address.forward_to[0].email == "maintainer@mail.example.org" &&
      cloudflare_email_routing_address.forward_to[0].account_id == "fedcba9876543210fedcba9876543210"
    )
    error_message = "the destination is an address of the zone's account"
  }
  assert {
    condition = (
      length(cloudflare_email_routing_rule.route) == 1 &&
      one([for m in cloudflare_email_routing_rule.route["support"].matcher : m.type == "literal" && m.field == "to" && m.value == "support@example.com"]) &&
      one([for a in cloudflare_email_routing_rule.route["support"].action : a.type == "forward" && toset(a.value) == toset(["maintainer@mail.example.org"])]) &&
      cloudflare_email_routing_rule.route["support"].enabled
    )
    error_message = "support@<domain> (the default route) forwards to the destination, by a literal match on the recipient"
  }
}

run "more_routes" {
  command = plan

  variables {
    email_routes = ["support", "abuse"]
  }

  assert {
    condition = (
      length(cloudflare_email_routing_rule.route) == 2 &&
      one([for m in cloudflare_email_routing_rule.route["abuse"].matcher : m.value == "abuse@example.com"])
    )
    error_message = "each email_routes local part is a rule of its own"
  }
}

run "placeholder_off" {
  command = plan

  variables {
    email_forward_to = "REPLACE_ME"
  }

  assert {
    condition = (
      length(cloudflare_email_routing_settings.zone) == 0 && length(cloudflare_email_routing_address.forward_to) == 0 &&
      length(cloudflare_email_routing_rule.route) == 0 && length(data.cloudflare_zone.email) == 0
    )
    error_message = "a placeholder destination: email routing off, nothing made"
  }
}

run "no_cloudflare" {
  command = plan

  variables {
    cloudflare_zone_id = ""
    cloudflare_domain  = ""
  }

  assert {
    condition     = length(cloudflare_email_routing_settings.zone) == 0 && length(cloudflare_email_routing_rule.route) == 0
    error_message = "without Cloudflare: no email routing"
  }
}

run "loop_refused" {
  command = plan

  variables {
    email_forward_to = "Owner@Example.com"
  }

  expect_failures = [cloudflare_email_routing_address.forward_to]
}

run "malformed_refused" {
  command = plan

  variables {
    email_forward_to = "maintainer"
  }

  expect_failures = [var.email_forward_to]
}

run "uppercase_route_refused" {
  command = plan

  variables {
    email_routes = ["Support"]
  }

  expect_failures = [var.email_routes]
}
