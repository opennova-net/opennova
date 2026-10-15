# A deployment besides prod (service.tf), planned in its own workspace against
# mock providers: offline, no credentials, nothing reaches AWS or Cloudflare.
# terraform test runs in the selected workspace, so these run apart from
# tests/, with the workspace `staging` (.github/workflows/infra.yml). From the
# repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -e TF_WORKSPACE=staging -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test -test-directory=tests/staging
# Staging gets an Elastic IP of its own, managed here, associated with its
# interface like prod's, and named by its records and infra.env. It never
# adopts one: an allocation id (a stray var-file holding prod's) stops the plan
# at the adopted address's precondition, before the address is read.

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

variables {
  edge_auth_secret   = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  db_master_password = "0123456789abcdef0123456789abcdef"
  cloudflare_zone_id = "0123456789abcdef0123456789abcdef"
  cloudflare_domain  = "example.com"
}

run "own_address" {
  command = plan

  assert {
    condition     = length(data.aws_eip.service) == 0 && length(aws_eip.service) == 1
    error_message = "staging adopts no address: it gets one of its own"
  }
  assert {
    condition     = aws_eip_association.service.allocation_id == "eipalloc-0fedcba9876543210" && aws_eip_association.service.network_interface_id == aws_network_interface.service.id && aws_eip_association.service.private_ip_address == "10.44.0.10"
    error_message = "its own address is associated with its interface at the fixed private address"
  }
  assert {
    condition = (
      output.public_ip == "3.3.3.3" &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_PUBLIC_HOST='3.3.3.3'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_DEPLOYMENT='staging'\n") &&
      aws_ssm_parameter.infra_env.name == "/opennova/staging/infra.env"
    )
    error_message = "infra.env (under /opennova/staging/) carries staging's own address"
  }
  assert {
    condition = (
      cloudflare_record.web_root[0].name == "staging" &&
      length(cloudflare_record.web_www) == 0 && cloudflare_record.web_root[0].content == "3.3.3.3"
    )
    error_message = "staging's record (staging; no www) points at its own address"
  }
  assert {
    condition     = aws_instance.service.tags["opennova:service"] == "staging" && aws_instance.service.tags["Name"] == "opennova-server-staging"
    error_message = "the instance carries the tag the toolbox finds staging's by, and staging's name"
  }
}

run "adopted_refused" {
  command = plan

  variables {
    service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  }

  expect_failures = [data.aws_eip.service]
}
