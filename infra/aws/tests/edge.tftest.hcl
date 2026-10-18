# Cloudflare in front of the portal (edge.tf), planned against mock providers
# whose values reach the plan: offline, no credentials, nothing reaches AWS or
# Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Proxied records: the deployment's own site records (prod's @ and www,
# staging's `staging`) and an Origin CA certificate (ECDSA) for its web
# hostnames alone (staging's names no prod hostname), its certificate and key
# handed to the portal through SSM SecureStrings under the deployment's prefix.
# Unproxied or without Cloudflare: no certificate. The update service's record
# (update, at the balancer; update.staging off prod) is never proxied, and
# goes only without a zone; so does the gate's (nw, an A record at the
# gate's zone-a address; nw.staging off prod). The zone's own settings and rules (Full strict,
# edge authentication, the webhook's WAF skip) are infra/zone's: none of them
# is made here.

mock_provider "aws" {
  override_during = plan

  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a", "us-east-1b", "us-east-1c"] }
  }
  mock_data "aws_ami" {
    defaults = { id = "ami-0123456789abcdef0", root_device_name = "/dev/sda1" }
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
  # The deployment's parameters as the toolbox left them: no release yet (a run
  # naming one overrides data.aws_ssm_parameters_by_path.deployment).
  mock_data "aws_ssm_parameters_by_path" {
    defaults = { names = [], values = [] }
  }
  mock_resource "aws_eip" {
    defaults = { allocation_id = "eipalloc-0fedcba9876543210", public_ip = "3.3.3.3" }
  }
  # Well-formed ARNs and ids for what the configuration hands to an ARN or id
  # argument (the provider's own validation reads them).
  mock_resource "aws_iam_instance_profile" {
    defaults = { arn = "arn:aws:iam::123456789012:instance-profile/mock" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::123456789012:role/mock" }
  }
  mock_resource "aws_cloudwatch_log_group" {
    defaults = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/mock" }
  }
  mock_resource "aws_sns_topic" {
    defaults = { arn = "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts" }
  }
  mock_resource "aws_ssm_parameter" {
    defaults = { arn = "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/mock" }
  }
  mock_resource "aws_db_instance" {
    defaults = { address = "opennova-prod-db.abcdefghijkl.us-east-1.rds.amazonaws.com", port = 5432, id = "db-MOCK" }
  }
  mock_resource "aws_elasticache_replication_group" {
    defaults = { primary_endpoint_address = "master.opennova-prod-cache.abcdef.use1.cache.amazonaws.com" }
  }
  mock_resource "aws_lb" {
    defaults = {
      arn        = "arn:aws:elasticloadbalancing:us-east-1:123456789012:loadbalancer/net/opennova-prod/0123456789abcdef"
      arn_suffix = "net/opennova-prod/0123456789abcdef"
      dns_name   = "opennova-prod-0123456789abcdef.elb.us-east-1.amazonaws.com"
    }
  }
  mock_resource "aws_lb_target_group" {
    defaults = {
      arn        = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-mock/0123456789abcdef"
      arn_suffix = "targetgroup/opennova-prod-mock/0123456789abcdef"
    }
  }
  mock_resource "aws_launch_template" {
    defaults = { arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0123456789abcdef0", id = "lt-0123456789abcdef0" }
  }
  mock_resource "aws_ecs_cluster" {
    defaults = { id = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod", arn = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod" }
  }
  mock_resource "aws_ecs_task_definition" {
    defaults = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/mock:1" }
  }
  mock_resource "aws_ecs_service" {
    defaults = { id = "arn:aws:ecs:us-east-1:123456789012:service/opennova-prod/mock" }
  }
  mock_resource "aws_service_discovery_private_dns_namespace" {
    defaults = { id = "ns-0123456789abcdef", arn = "arn:aws:servicediscovery:us-east-1:123456789012:namespace/ns-0123456789abcdef" }
  }
  mock_resource "aws_service_discovery_service" {
    defaults = { id = "srv-0123456789abcdef", arn = "arn:aws:servicediscovery:us-east-1:123456789012:service/srv-0123456789abcdef" }
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

# The ids and ARNs the assertions compare, known at plan.
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
  target          = aws_security_group.program["gate"]
  override_during = plan
  values          = { id = "sg-0gate" }
}
override_resource {
  target          = aws_security_group.program["lobby"]
  override_during = plan
  values          = { id = "sg-0lobby" }
}
override_resource {
  target          = aws_security_group.program["web"]
  override_during = plan
  values          = { id = "sg-0web" }
}
override_resource {
  target          = aws_security_group.program["fleet"]
  override_during = plan
  values          = { id = "sg-0fleet" }
}
override_resource {
  target          = aws_security_group.program["worker"]
  override_during = plan
  values          = { id = "sg-0worker" }
}
override_resource {
  target          = aws_security_group.program["data"]
  override_during = plan
  values          = { id = "sg-0data" }
}
override_resource {
  target          = aws_security_group.lb
  override_during = plan
  values          = { id = "sg-0lb" }
}
override_resource {
  target          = aws_security_group.db
  override_during = plan
  values          = { id = "sg-0db" }
}
override_resource {
  target          = aws_security_group.backup
  override_during = plan
  values          = { id = "sg-0backup" }
}
override_resource {
  target          = aws_lb_target_group.public["gate"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-gate/0123456789abcdef", arn_suffix = "targetgroup/opennova-prod-gate/0123456789abcdef" }
}
override_resource {
  target          = aws_lb_target_group.public["https"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-https/0123456789abcdef", arn_suffix = "targetgroup/opennova-prod-https/0123456789abcdef" }
}
override_resource {
  target          = aws_lb_target_group.public["http"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-http/0123456789abcdef", arn_suffix = "targetgroup/opennova-prod-http/0123456789abcdef" }
}
override_resource {
  target          = aws_eip.lb_b
  override_during = plan
  values          = { allocation_id = "eipalloc-0000000000000000b", public_ip = "5.5.5.5" }
}
override_resource {
  target          = aws_ecs_task_definition.program["gate"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-gate:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["lobby"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-lobby:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["web"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-fleet:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-data:1" }
}
override_resource {
  target          = aws_ecs_task_definition.backup
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup:1" }
}
override_resource {
  target          = aws_iam_role.program["gate"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-gate" }
}
override_resource {
  target          = aws_iam_role.program["lobby"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-lobby" }
}
override_resource {
  target          = aws_iam_role.program["web"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-web" }
}
override_resource {
  target          = aws_iam_role.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-fleet" }
}
override_resource {
  target          = aws_iam_role.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-worker" }
}
override_resource {
  target          = aws_iam_role.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-data" }
}
override_resource {
  target          = aws_iam_role.execution
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-execution" }
}
override_resource {
  target          = aws_iam_role.backup
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-backup" }
}
override_resource {
  target          = aws_iam_role.backup_schedule
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-backup-schedule" }
}

variables {
  deployment                = "prod"
  aws_account_id            = "123456789012"
  region                    = "us-east-1"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
  cloudflare_zone_id        = "0123456789abcdef0123456789abcdef"
  cloudflare_domain         = "example.com"
}

run "edge" {
  command = plan

  assert {
    condition = (
      cloudflare_record.web_root[0].name == "@" && cloudflare_record.web_root[0].type == "CNAME" && cloudflare_record.web_root[0].proxied &&
      length(cloudflare_record.web_www) == 1 && cloudflare_record.web_www[0].name == "www"
    )
    error_message = "prod's site records: the apex and www, proxied"
  }
  assert {
    condition     = tls_private_key.origin[0].algorithm == "ECDSA" && tls_private_key.origin[0].ecdsa_curve == "P256"
    error_message = "the origin's key: ECDSA P-256"
  }
  assert {
    condition = (
      toset(cloudflare_origin_ca_certificate.origin[0].hostnames) == toset(["example.com", "www.example.com"]) &&
      cloudflare_origin_ca_certificate.origin[0].request_type == "origin-ecc" &&
      tls_cert_request.origin[0].subject[0].common_name == "example.com"
    )
    error_message = "an Origin CA certificate for prod's web hostnames alone (no wildcard that would cover staging's)"
  }
  assert {
    condition = (
      aws_ssm_parameter.origin_tls_cert[0].name == "/opennova/prod/tls/ORIGIN_TLS_CERT" && aws_ssm_parameter.origin_tls_cert[0].type == "SecureString" &&
      aws_ssm_parameter.origin_tls_key[0].name == "/opennova/prod/tls/ORIGIN_TLS_KEY" && aws_ssm_parameter.origin_tls_key[0].type == "SecureString" &&
      base64decode(aws_ssm_parameter.origin_tls_cert[0].value) == "-----BEGIN CERTIFICATE-----\nMOCK\n-----END CERTIFICATE-----\n"
    )
    error_message = "the certificate and key reach the portal as base64 SecureStrings under /opennova/<deployment>/tls/"
  }
  assert {
    condition = (
      cloudflare_record.update[0].name == "update" && cloudflare_record.update[0].type == "CNAME" &&
      cloudflare_record.update[0].content == "opennova-prod-0123456789abcdef.elb.us-east-1.amazonaws.com" &&
      !cloudflare_record.update[0].proxied && cloudflare_record.update[0].ttl == 300 &&
      output.update_host == "update.example.com" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_UPDATE_HOST"] == "update.example.com"
    )
    error_message = "the update service's record is an unproxied CNAME at the balancer (retail UPDATE.EXE speaks plain TCP to it), the name web serves"
  }
  assert {
    condition = (
      cloudflare_record.novaworld[0].name == "nw" && cloudflare_record.novaworld[0].type == "A" &&
      cloudflare_record.novaworld[0].content == "54.162.79.147" &&
      !cloudflare_record.novaworld[0].proxied && cloudflare_record.novaworld[0].ttl == 300 &&
      output.novaworld_host == "nw.example.com"
    )
    error_message = "the gate's record is nw, an unproxied A record at the gate's own address (the gate speaks UDP; zone b never answers for it)"
  }
}

# Staging's records and certificate: its own first-level name alone.
run "staging_edge" {
  command = plan

  variables {
    deployment                = "staging"
    service_eip_allocation_id = ""
  }

  assert {
    condition     = cloudflare_record.web_root[0].name == "staging" && length(cloudflare_record.web_www) == 0
    error_message = "staging's site record is staging.<domain> alone: no apex, no www"
  }
  assert {
    condition = (
      toset(cloudflare_origin_ca_certificate.origin[0].hostnames) == toset(["staging.example.com"]) &&
      tls_cert_request.origin[0].subject[0].common_name == "staging.example.com"
    )
    error_message = "staging's origin certificate names staging.<domain> alone: its key is good for no prod hostname"
  }
  assert {
    condition     = aws_ssm_parameter.origin_tls_key[0].name == "/opennova/staging/tls/ORIGIN_TLS_KEY"
    error_message = "staging's certificate reaches staging's portal alone, under /opennova/staging/tls/"
  }
  assert {
    condition = (
      cloudflare_record.update[0].name == "update.staging" && !cloudflare_record.update[0].proxied &&
      output.update_host == "update.staging.example.com" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_UPDATE_HOST"] == "update.staging.example.com"
    )
    error_message = "staging's update record is update.staging, unproxied, and is staging's web's update host"
  }
  assert {
    condition = (
      cloudflare_record.novaworld[0].name == "nw.staging" && !cloudflare_record.novaworld[0].proxied &&
      cloudflare_record.novaworld[0].content == "3.3.3.3" && output.novaworld_host == "nw.staging.example.com"
    )
    error_message = "staging's gate record is nw.staging, unproxied, at staging's own gate address"
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
      !strcontains(aws_ecs_task_definition.program["web"].container_definitions, "ORIGIN_TLS_CERT") &&
      length(cloudflare_record.update) == 1 && !cloudflare_record.update[0].proxied &&
      length(cloudflare_record.novaworld) == 1 && !cloudflare_record.novaworld[0].proxied
    )
    error_message = "unproxied records: no origin certificate (the portal reads none); the update and gate records stay (they were never proxied)"
  }
}

run "no_cloudflare" {
  command = plan

  variables {
    cloudflare_zone_id = ""
    cloudflare_domain  = ""
  }

  assert {
    condition = (
      length(aws_ssm_parameter.origin_tls_cert) == 0 && length(cloudflare_record.web_root) == 0 &&
      length(cloudflare_record.update) == 0 && output.update_host == null &&
      length(cloudflare_record.novaworld) == 0 && output.novaworld_host == null &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_UPDATE_HOST"] == ""
    )
    error_message = "without Cloudflare: none of it, no update or gate record, and web's update host empty (its listener stays off)"
  }
}
