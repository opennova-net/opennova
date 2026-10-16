# Outbound mail through Amazon SES (mail.tf), planned against mock providers
# whose values reach the plan: offline, no credentials, nothing reaches AWS or
# Cloudflare (tests/email.tftest.hcl says how these run). With a sender, prod
# makes the domain's SES identity with its custom MAIL FROM domain, the three
# DKIM CNAMEs and the MAIL FROM's MX and SPF records in the zone (DNS only), an
# identity for the forward_to address (the sandbox's recipient), the service
# role's one permission (ses:SendEmail through those identities, from the sender
# alone) and the sender and region in infra.env. With none (the default), or
# without Cloudflare, none of it; a malformed sender is refused. (tests/staging:
# off prod, the role's permission and infra.env alone.)

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

# Easy DKIM's three tokens, which SES hands back when it makes the identity.
override_resource {
  target          = aws_sesv2_email_identity.domain
  override_during = plan
  values = {
    dkim_signing_attributes = { tokens = ["tok1", "tok2", "tok3"] }
  }
}

variables {
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cloudflare_zone_id        = "0123456789abcdef0123456789abcdef"
  cloudflare_domain         = "example.com"
  edge_auth_secret          = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  email_forward_to          = "maintainer@mail.example.org"
  mail_sender               = "support"
}

run "mail" {
  command = plan

  assert {
    condition = (
      aws_sesv2_email_identity.domain[0].email_identity == "example.com" &&
      aws_sesv2_email_identity_mail_from_attributes.domain[0].mail_from_domain == "bounce.example.com" &&
      aws_sesv2_email_identity_mail_from_attributes.domain[0].behavior_on_mx_failure == "USE_DEFAULT_VALUE"
    )
    error_message = "the domain's identity, its MAIL FROM domain bounce.<domain>"
  }
  assert {
    condition = (
      length(cloudflare_record.mail_dkim) == 3 &&
      alltrue([for i, r in cloudflare_record.mail_dkim :
        r.type == "CNAME" && !r.proxied && r.zone_id == "0123456789abcdef0123456789abcdef" &&
        r.name == "tok${i + 1}._domainkey" && r.content == "tok${i + 1}.dkim.amazonses.com"
      ])
    )
    error_message = "Easy DKIM's three CNAMEs, DNS only, in the zone"
  }
  assert {
    condition = (
      cloudflare_record.mail_from_mx[0].name == "bounce" && cloudflare_record.mail_from_mx[0].type == "MX" &&
      cloudflare_record.mail_from_mx[0].content == "feedback-smtp.us-east-1.amazonses.com" &&
      cloudflare_record.mail_from_mx[0].priority == 10 &&
      cloudflare_record.mail_from_spf[0].name == "bounce" && cloudflare_record.mail_from_spf[0].type == "TXT" &&
      cloudflare_record.mail_from_spf[0].content == "v=spf1 include:amazonses.com ~all"
    )
    error_message = "the MAIL FROM domain's MX (SES's feedback endpoint) and SPF"
  }
  assert {
    condition     = aws_sesv2_email_identity.forward_to[0].email_identity == "maintainer@mail.example.org"
    error_message = "the forward_to address is an identity (the sandbox's recipient)"
  }
  assert {
    condition = (
      length(data.aws_iam_policy_document.mail[0].statement) == 1 &&
      one([for st in data.aws_iam_policy_document.mail[0].statement :
        toset(st.actions) == toset(["ses:SendEmail"]) &&
        toset(st.resources) == toset([
          "arn:aws:ses:us-east-1:123456789012:identity/example.com",
          "arn:aws:ses:us-east-1:123456789012:identity/maintainer@mail.example.org",
        ]) &&
        one([for c in st.condition : c.test == "StringEquals" && c.variable == "ses:FromAddress" && toset(c.values) == toset(["support@example.com"])])
      ]) &&
      aws_iam_role_policy.mail[0].name == "opennova-server-mail"
    )
    error_message = "the service role sends through the two identities, from support@<domain> alone"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_MAIL_FROM='support@example.com'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_MAIL_REGION='us-east-1'\n")
    )
    error_message = "infra.env names the sender and SES's region"
  }
}

run "no_forward_to" {
  command = plan

  variables {
    email_forward_to = "REPLACE_ME"
  }

  assert {
    condition = (
      length(aws_sesv2_email_identity.domain) == 1 && length(aws_sesv2_email_identity.forward_to) == 0 &&
      one([for st in data.aws_iam_policy_document.mail[0].statement :
        toset(st.resources) == toset(["arn:aws:ses:us-east-1:123456789012:identity/example.com"])
      ])
    )
    error_message = "with no destination, the domain's identity alone"
  }
}

run "off" {
  command = plan

  variables {
    mail_sender = ""
  }

  assert {
    condition = (
      length(aws_sesv2_email_identity.domain) == 0 && length(aws_sesv2_email_identity.forward_to) == 0 &&
      length(aws_sesv2_email_identity_mail_from_attributes.domain) == 0 && length(cloudflare_record.mail_dkim) == 0 &&
      length(cloudflare_record.mail_from_mx) == 0 && length(cloudflare_record.mail_from_spf) == 0 &&
      length(aws_iam_role_policy.mail) == 0 && !strcontains(aws_ssm_parameter.infra_env.value, "ONNET_MAIL_")
    )
    error_message = "no sender: no mail, nothing made"
  }
}

run "no_cloudflare" {
  command = plan

  variables {
    cloudflare_zone_id = ""
    cloudflare_domain  = ""
  }

  assert {
    condition     = length(aws_sesv2_email_identity.domain) == 0 && length(aws_iam_role_policy.mail) == 0
    error_message = "without Cloudflare: no mail"
  }
}

run "uppercase_sender_refused" {
  command = plan

  variables {
    mail_sender = "Support"
  }

  expect_failures = [var.mail_sender]
}
