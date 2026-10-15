# The service host (service.tf, service_ssm.tf), planned against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare. From the
# repo root (the configuration reads deploy/host/ beside infra/):
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Prod adopts its Elastic IP by allocation id (no managed address, the
# association on the host's network interface at its fixed private address); a
# deployment with no allocation id gets a managed one. No SSH: no rule on port
# 22, the web port from Cloudflare's ranges alone, the retail ports from
# everywhere. The instance requires IMDSv2 with a hop limit of 1; its role reads
# its own deployment's parameters alone. infra.env carries the public address;
# the release parameter starts at none; the converge document holds
# deploy/host/converge.sh.

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
  edge_auth_secret          = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cloudflare_zone_id        = "0123456789abcdef0123456789abcdef"
  cloudflare_domain         = "example.com"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
}

run "adopted_eip" {
  command = plan

  variables {
    service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  }

  assert {
    condition     = length(aws_eip.service) == 0
    error_message = "an adopted Elastic IP is never managed (no plan can release it)"
  }
  assert {
    condition     = aws_eip_association.service.allocation_id == "eipalloc-0123456789abcdef0" && aws_eip_association.service.network_interface_id == aws_network_interface.service.id && aws_eip_association.service.private_ip_address == "10.44.0.10"
    error_message = "the adopted address is associated with the host's network interface at its fixed private address"
  }
  assert {
    condition     = output.public_ip == "54.162.79.147" && output.service_private_ip == "10.44.0.10"
    error_message = "the outputs name the adopted address and the fixed private one"
  }
  assert {
    condition     = length(aws_network_interface.service.private_ips) == 1 && contains(aws_network_interface.service.private_ips, "10.44.0.10") && aws_network_interface.service.subnet_id == aws_subnet.public["a"].id
    error_message = "the network interface holds 10.44.0.10 in public subnet a"
  }
  assert {
    condition     = aws_instance.service.metadata_options[0].http_tokens == "required" && aws_instance.service.metadata_options[0].http_put_response_hop_limit == 1
    error_message = "the host requires IMDSv2 with a hop limit of 1 (no bridge container reaches it)"
  }
  assert {
    condition     = one([for n in aws_instance.service.network_interface : n.network_interface_id if n.device_index == 0]) == aws_network_interface.service.id && aws_instance.service.tags["opennova:service"] == "prod"
    error_message = "the instance takes the interface, and the tag the toolbox finds it by"
  }
  assert {
    condition     = aws_instance.service.root_block_device[0].encrypted
    error_message = "an encrypted root volume"
  }
  assert {
    condition     = alltrue([for r in aws_vpc_security_group_ingress_rule.service : r.from_port != 22 && r.to_port != 22 && r.from_port != 80])
    error_message = "no SSH, no port 80"
  }
  assert {
    condition     = length([for r in aws_vpc_security_group_ingress_rule.service : r if r.from_port == 443]) == 2 && alltrue([for r in aws_vpc_security_group_ingress_rule.service : contains(["173.245.48.0/20", "103.21.244.0/22"], r.cidr_ipv4) if r.from_port == 443])
    error_message = "the web port (443) admits Cloudflare's ranges alone"
  }
  assert {
    condition = (
      length([for r in aws_vpc_security_group_ingress_rule.service : r if r.from_port == 7597 && r.ip_protocol == "udp" && r.cidr_ipv4 == "0.0.0.0/0"]) == 1 &&
      length([for r in aws_vpc_security_group_ingress_rule.service : r if r.from_port == 64206 && r.ip_protocol == "udp" && r.cidr_ipv4 == "0.0.0.0/0"]) == 1 &&
      length([for r in aws_vpc_security_group_ingress_rule.service : r if r.from_port == 8080 && r.ip_protocol == "tcp" && r.cidr_ipv4 == "0.0.0.0/0"]) == 1
    )
    error_message = "the retail ports (gate UDP 7597, NovaWorld UDP 64206, HTTP 8080) are open to all"
  }
  assert {
    condition     = length(aws_vpc_security_group_ingress_rule.service) == 5
    error_message = "the service admits the three retail ports and Cloudflare's two (mock) ranges, nothing else"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_PUBLIC_HOST='54.162.79.147'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_SITE_URL='https://example.com'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_DEPLOYMENT='prod'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "BACKUP_PREFIX='novaworld'\n") &&
      aws_ssm_parameter.infra_env.name == "/opennova/prod/infra.env" && aws_ssm_parameter.infra_env.type == "String"
    )
    error_message = "infra.env carries the public address, the site, the deployment and its backup prefix"
  }
  assert {
    condition     = aws_ssm_parameter.release.name == "/opennova/prod/release" && aws_ssm_parameter.release.value == "none"
    error_message = "the release parameter starts at none"
  }
  assert {
    condition     = strcontains(aws_ssm_document.converge.content, "opennova-converge") && aws_ssm_document.converge.document_type == "Command"
    error_message = "the converge document holds deploy/host/converge.sh"
  }
  assert {
    condition = alltrue([
      for st in data.aws_iam_policy_document.service.statement :
      toset(st.resources) == toset(["arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod", "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/*"])
      if length([for a in st.actions : a if startswith(a, "ssm:GetParameter")]) > 0
    ])
    error_message = "the host reads its own deployment's parameters alone"
  }
  assert {
    condition = alltrue([
      for st in data.aws_iam_policy_document.service.statement : !contains(st.resources, "*")
      if st.sid != "SsmAgent"
    ])
    error_message = "every statement but the SSM agent's is scoped to a resource"
  }
  assert {
    condition     = cloudflare_record.web_root[0].name == "@" && cloudflare_record.web_www[0].name == "www" && cloudflare_record.game[0].name == "game" && cloudflare_record.web_root[0].content == "54.162.79.147"
    error_message = "prod's records (@, www, game) point at the adopted address"
  }
}

# Prod without an allocation id gets a new address: the plan says so (the check
# below), and the toolbox's apply refuses to destroy the old one.
run "managed_eip" {
  command = plan

  variables {
    service_eip_allocation_id = ""
  }

  expect_failures = [check.prod_adopts_its_elastic_ip]

  assert {
    condition     = length(aws_eip.service) == 1 && aws_eip_association.service.allocation_id == "eipalloc-0fedcba9876543210"
    error_message = "a deployment that adopts none gets a managed address, associated the same way"
  }
  assert {
    condition     = output.public_ip == "3.3.3.3" && strcontains(aws_ssm_parameter.infra_env.value, "ONNET_PUBLIC_HOST='3.3.3.3'\n")
    error_message = "the managed address is the public one"
  }
}

run "no_cloudflare" {
  command = plan

  variables {
    service_eip_allocation_id = "eipalloc-0123456789abcdef0"
    cloudflare_zone_id        = ""
    cloudflare_domain         = ""
  }

  assert {
    condition     = length(aws_vpc_security_group_ingress_rule.service) == 3 && length(cloudflare_record.web_root) == 0
    error_message = "without Cloudflare: the retail ports alone, and no record"
  }
}

run "private_address" {
  command = plan

  variables {
    service_eip_allocation_id = "eipalloc-0123456789abcdef0"
    service_private_ip_host   = 11
  }

  assert {
    condition     = output.service_private_ip == "10.44.0.11"
    error_message = "service_private_ip_host picks the host's address in public subnet a"
  }
}

run "malformed_allocation_refused" {
  command = plan

  variables {
    service_eip_allocation_id = "54.162.79.147"
  }

  expect_failures = [var.service_eip_allocation_id]
}

run "burstable_host_refused" {
  command = plan

  variables {
    instance_type = "t3.large"
  }

  expect_failures = [var.instance_type]
}
