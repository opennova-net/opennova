# The service's shared pieces (service.tf, service_ssm.tf): the gate's address
# and what the deploy toolbox reads, planned against mock providers: offline,
# no credentials, nothing reaches AWS or Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Prod adopts its Elastic IP by allocation id (no managed address): it is the
# load balancer's zone-a address, and the programs announce it as the gate's
# and web's (ONNET_PUBLIC_HOST, ONNET_WEB_HOST); a prod plan naming none stops
# at the address's precondition: prod never makes one (staging_service.tftest.hcl:
# staging gets a managed address and never adopts one). Every name carries the
# deployment (opennova-prod-*), and the operations bucket the deployment and
# its account. No SSH and no port
# 80 in any group. infra.env carries what the toolbox needs (the cluster, the
# templates, the one-off tasks' network, the registry, the bucket) and nothing
# a task reads; the release parameter starts at none; nothing is left of the
# instances' converge.

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
  target          = aws_security_group.program["controller"]
  override_during = plan
  values          = { id = "sg-0controller" }
}
override_resource {
  target          = aws_security_group.program["worker"]
  override_during = plan
  values          = { id = "sg-0worker" }
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
  target          = aws_ecs_task_definition.program["controller"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-controller:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:1" }
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
  target          = aws_iam_role.program["controller"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-controller" }
}
override_resource {
  target          = aws_iam_role.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-worker" }
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
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
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
    condition     = length(aws_eip.gate) == 0
    error_message = "an adopted Elastic IP is never managed (no plan can release it)"
  }
  assert {
    condition     = output.public_ip == "54.162.79.147" && output.service_eip_allocation_id == "eipalloc-0123456789abcdef0"
    error_message = "the outputs name the adopted address"
  }
  assert {
    condition = (
      length(aws_ec2_tag.gate_eip) == 1 && aws_ec2_tag.gate_eip[0].key == "opennova:deployment" &&
      aws_ec2_tag.gate_eip[0].value == "opennova-prod"
    )
    error_message = "the adopted address carries the deployment's tag"
  }
  assert {
    condition = alltrue([
      for p in ["gate", "lobby", "web", "controller", "worker"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_PUBLIC_HOST"] == "54.162.79.147" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_WEB_HOST"] == "54.162.79.147"
    ])
    error_message = "every program names the adopted address as the gate's and web's: no retail client follows a DNS name"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_DEPLOYMENT='prod'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_CLUSTER='opennova-prod'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_TASK_DEFINITIONS='{\"backup\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup:1\",\"controller\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-controller:1\",\"gate\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-gate:1\",\"lobby\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-lobby:1\",\"web\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:1\",\"worker\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:1\"}'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_TASK_SUBNETS='subnet-0public0a,subnet-0public0b'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_BACKUP_SECURITY_GROUP='sg-0backup'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_IMAGE_REGISTRY='ghcr.io/opennova-net'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_OPS_BUCKET='opennova-prod-ops-123456789012'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_LOG_GROUP='/opennova/prod/service'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "BACKUP_PREFIX='novaworld'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_PUBLIC_HOST='54.162.79.147'\n") &&
      aws_ssm_parameter.infra_env.name == "/opennova/prod/infra.env" && aws_ssm_parameter.infra_env.type == "String"
    )
    error_message = "infra.env hands the toolbox the cluster, each task definition, the one-off tasks' network, the registry, the bucket, the log group and the gate's address"
  }
  assert {
    condition = (
      !strcontains(aws_ssm_parameter.infra_env.value, "DATABASE_URL") && !strcontains(aws_ssm_parameter.infra_env.value, "ONNET_REDIS_URL") &&
      !strcontains(aws_ssm_parameter.infra_env.value, "CONVERGE") && !strcontains(aws_ssm_parameter.infra_env.value, "GATE_ALLOCATION")
    )
    error_message = "infra.env is the toolbox's alone: the tasks read their task definitions' environment, and no instance converges"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_ACCOUNT_ID='123456789012'\n") &&
      aws_s3_bucket.ops.bucket == "opennova-prod-ops-123456789012" &&
      aws_ecs_cluster.main.name == "opennova-prod" && aws_db_instance.main.identifier == "opennova-prod-db" &&
      aws_iam_role.execution.name == "opennova-prod-execution" &&
      alltrue([for p, r in aws_iam_role.program : r.name == "opennova-prod-${p}"]) &&
      aws_lb.public.name == "opennova-prod" && aws_elasticache_replication_group.cache.replication_group_id == "opennova-prod-cache"
    )
    error_message = "prod's names carry prod (opennova-prod-*), its bucket prod and its account, and infra.env names the account"
  }
  assert {
    condition     = aws_ssm_parameter.release.name == "/opennova/prod/release" && aws_ssm_parameter.release.value == "none"
    error_message = "the release parameter starts at none"
  }
  assert {
    condition = alltrue(concat(
      [for r in aws_vpc_security_group_ingress_rule.from_lb : r.from_port != 22 && r.to_port != 22 && r.from_port != 80],
      [for r in aws_vpc_security_group_ingress_rule.lobby : r.from_port != 22 && r.from_port != 80],
      [for r in aws_vpc_security_group_ingress_rule.lb : r.from_port != 22 && r.from_port != 80],
    ))
    error_message = "no SSH, no port 80"
  }
  assert {
    condition = (
      cloudflare_record.web_root[0].name == "@" && cloudflare_record.web_www[0].name == "www" &&
      cloudflare_record.web_root[0].type == "CNAME" && cloudflare_record.web_root[0].content == "opennova-prod-0123456789abcdef.elb.us-east-1.amazonaws.com" &&
      cloudflare_record.web_root[0].proxied
    )
    error_message = "prod's records (@, www) are proxied CNAMEs at the load balancer"
  }
}

# Prod without an allocation id would get a new address: the plan stops at the
# managed address's precondition instead. Prod never makes one.
run "managed_eip" {
  command = plan

  variables {
    service_eip_allocation_id = ""
  }

  expect_failures = [aws_eip.gate]
}

run "no_cloudflare" {
  command = plan

  variables {
    service_eip_allocation_id = "eipalloc-0123456789abcdef0"
    cloudflare_zone_id        = ""
    cloudflare_domain         = ""
  }

  assert {
    condition = (
      length(aws_vpc_security_group_ingress_rule.lb) == 2 && length(cloudflare_record.web_root) == 0 &&
      !strcontains(aws_ecs_task_definition.program["web"].container_definitions, "ONNET_SITE_URL") &&
      strcontains(aws_ecs_task_definition.program["web"].container_definitions, "\"ONNET_COOKIE_SECURE\",\"value\":\"0\"")
    )
    error_message = "without Cloudflare: the gate's and the retail port's rules alone, no record, no site URL and no Secure cookie"
  }
}

run "malformed_allocation_refused" {
  command = plan

  variables {
    service_eip_allocation_id = "54.162.79.147"
  }

  expect_failures = [var.service_eip_allocation_id]
}
