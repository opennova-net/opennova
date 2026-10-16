# The cache (cache.tf), planned against mock providers: offline, no
# credentials, nothing reaches AWS or Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Valkey 8 on ElastiCache: a primary and a replica in the two zones' private
# subnets, failing over on its own; TLS required, the AUTH token from the
# vault, encrypted at rest, no snapshots, volatile-lru; reached on 6379 from
# web's security group alone. Web's environment names a rediss:// URL with no
# token in it, and no other program's names the cache; each node's evictions
# and CPU credits alarm. A short token is refused.

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

override_resource {
  target          = aws_security_group.cache
  override_during = plan
  values          = { id = "sg-0cache" }
}

variables {
  deployment                = "prod"
  aws_account_id            = "123456789012"
  region                    = "us-east-1"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
}

run "cache" {
  command = plan

  assert {
    condition = (
      aws_elasticache_replication_group.cache.engine == "valkey" && aws_elasticache_replication_group.cache.engine_version == "8.0" &&
      aws_elasticache_replication_group.cache.node_type == "cache.t4g.micro" && aws_elasticache_replication_group.cache.port == 6379
    )
    error_message = "Valkey 8.0 on cache.t4g.micro, port 6379"
  }
  assert {
    condition = (
      aws_elasticache_replication_group.cache.num_cache_clusters == 2 &&
      aws_elasticache_replication_group.cache.preferred_cache_cluster_azs == tolist(["us-east-1a", "us-east-1b"]) &&
      aws_elasticache_replication_group.cache.automatic_failover_enabled && aws_elasticache_replication_group.cache.multi_az_enabled
    )
    error_message = "a primary and a replica, one in each zone, failing over on its own"
  }
  assert {
    condition = (
      aws_elasticache_replication_group.cache.transit_encryption_enabled && aws_elasticache_replication_group.cache.transit_encryption_mode == "required" &&
      aws_elasticache_replication_group.cache.at_rest_encryption_enabled &&
      aws_elasticache_replication_group.cache.auth_token == "fedcba9876543210fedcba9876543210" &&
      aws_elasticache_replication_group.cache.auth_token_update_strategy == "ROTATE"
    )
    error_message = "TLS required, encrypted at rest, the vault's AUTH token (rotated, never cut over)"
  }
  assert {
    condition     = aws_elasticache_replication_group.cache.snapshot_retention_limit == 0
    error_message = "no snapshots: nothing in the cache outlives its TTL"
  }
  assert {
    condition = (
      aws_elasticache_parameter_group.cache.family == "valkey8" &&
      length([for p in aws_elasticache_parameter_group.cache.parameter : p if p.name == "maxmemory-policy" && p.value == "volatile-lru"]) == 1 &&
      aws_elasticache_replication_group.cache.parameter_group_name == aws_elasticache_parameter_group.cache.name
    )
    error_message = "the group's parameters: a full node evicts the least recently used key (every key has a TTL)"
  }
  assert {
    condition     = toset(aws_elasticache_subnet_group.cache.subnet_ids) == toset(["subnet-0private0a", "subnet-0private0b"])
    error_message = "the cache lives in the private subnets"
  }
  assert {
    condition = (
      aws_elasticache_replication_group.cache.security_group_ids == toset(["sg-0cache"]) &&
      aws_vpc_security_group_ingress_rule.cache_from_web.security_group_id == "sg-0cache" &&
      aws_vpc_security_group_ingress_rule.cache_from_web.from_port == 6379 && aws_vpc_security_group_ingress_rule.cache_from_web.to_port == 6379 &&
      aws_vpc_security_group_ingress_rule.cache_from_web.ip_protocol == "tcp" &&
      aws_vpc_security_group_ingress_rule.cache_from_web.referenced_security_group_id == "sg-0web" &&
      aws_vpc_security_group_ingress_rule.cache_from_web.cidr_ipv4 == null
    )
    error_message = "the cache admits 6379 from web's security group alone, never by range"
  }
  assert {
    condition = (
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_REDIS_URL"] == "rediss://master.opennova-prod-cache.abcdef.use1.cache.amazonaws.com:6379" &&
      alltrue([for p, d in aws_ecs_task_definition.program : !strcontains(d.container_definitions, "fedcba9876543210fedcba9876543210")]) &&
      alltrue([for p, d in aws_ecs_task_definition.program : !strcontains(d.container_definitions, "ONNET_REDIS_URL") if p != "web"]) &&
      length([for s in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].secrets : s if s.name == "ONNET_REDIS_PASSWORD" && s.valueFrom == "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/secret/ONNET_REDIS_PASSWORD"]) == 1
    )
    error_message = "web's environment names the cache's TLS URL, never its token (an SSM secret), and no other program's names the cache"
  }
  assert {
    condition     = output.cache_endpoint == "master.opennova-prod-cache.abcdef.use1.cache.amazonaws.com" && output.cache_port == 6379
    error_message = "the outputs name the endpoint and the port"
  }
  assert {
    condition = (
      toset(keys(aws_cloudwatch_metric_alarm.cache_evictions)) == toset(["opennova-prod-cache-001", "opennova-prod-cache-002"]) &&
      aws_cloudwatch_metric_alarm.cache_evictions["opennova-prod-cache-001"].metric_name == "Evictions" &&
      aws_cloudwatch_metric_alarm.cache_evictions["opennova-prod-cache-001"].threshold == 0 &&
      length(aws_cloudwatch_metric_alarm.cache_cpu_credits) == 2
    )
    error_message = "each node's evictions and its CPU credits (a burstable node) alarm"
  }
}

# A node type with no credits: no credit alarm.
run "fixed_performance_nodes" {
  command = plan

  variables {
    cache_node_type = "cache.m7g.large"
  }

  assert {
    condition     = length(aws_cloudwatch_metric_alarm.cache_cpu_credits) == 0 && length(aws_cloudwatch_metric_alarm.cache_evictions) == 2
    error_message = "a non-burstable node: the evictions alarms alone"
  }
}

run "short_token_refused" {
  command = plan

  variables {
    cache_auth_token = "short"
  }

  expect_failures = [var.cache_auth_token]
}
