# Outbound mail through Amazon SES (mail.tf) off prod, planned in the staging
# workspace against mock providers (tests/staging/service.tftest.hcl says how
# these run). The domain's identity, its records and the forward_to identity
# are prod's: staging makes none of them, and its service role sends through
# prod's identities by ARN, from the sender alone.

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
  mock_resource "aws_eip" {
    defaults = { allocation_id = "eipalloc-0fedcba9876543210", public_ip = "3.3.3.3" }
  }
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
    defaults = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/staging/mock" }
  }
  mock_resource "aws_ssm_document" {
    defaults = { arn = "arn:aws:ssm:us-east-1:123456789012:document/opennova-server-staging-converge" }
  }
  mock_resource "aws_sns_topic" {
    defaults = { arn = "arn:aws:sns:us-east-1:123456789012:opennova-server-staging-alerts" }
  }
  mock_resource "aws_db_instance" {
    defaults = { address = "opennova-server-staging-db.abcdefghijkl.us-east-1.rds.amazonaws.com", port = 5432, id = "db-MOCK" }
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
  edge_auth_secret   = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  db_master_password = "0123456789abcdef0123456789abcdef"
  cloudflare_zone_id = "0123456789abcdef0123456789abcdef"
  cloudflare_domain  = "example.com"
  email_forward_to   = "maintainer@mail.example.org"
  mail_sender        = "support"
}

run "mail_identities_are_prods" {
  command = plan

  assert {
    condition = (
      length(aws_sesv2_email_identity.domain) == 0 && length(aws_sesv2_email_identity.forward_to) == 0 &&
      length(aws_sesv2_email_identity_mail_from_attributes.domain) == 0 && length(cloudflare_record.mail_dkim) == 0 &&
      length(cloudflare_record.mail_from_mx) == 0 && length(cloudflare_record.mail_from_spf) == 0
    )
    error_message = "the domain's identity and records are prod's: staging makes none of them"
  }
  assert {
    condition = (
      one([for st in data.aws_iam_policy_document.mail[0].statement :
        toset(st.resources) == toset([
          "arn:aws:ses:us-east-1:123456789012:identity/example.com",
          "arn:aws:ses:us-east-1:123456789012:identity/maintainer@mail.example.org",
        ])
      ]) &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_MAIL_FROM='support@example.com'\n")
    )
    error_message = "staging's service sends through prod's identities, as support@<domain>"
  }
}
