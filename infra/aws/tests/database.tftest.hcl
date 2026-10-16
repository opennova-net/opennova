# The service database (database.tf), planned and applied against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare. From the
# repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# PostgreSQL 17 in the private subnets on db.t4g.small, Multi-AZ (or,
# off, single-AZ in private subnet a's zone), never public, encrypted, TLS
# forced, max_connections sized for the programs' pools at their maxima with a
# deployment's extra tasks, 14 days of point-in-time recovery, a final snapshot
# and deletion protection; reached from the five programs' security groups and
# the backup task's alone; its master password write-only; every program's
# environment and the backup task's hand it a verify-full URL with no password
# in it, and the pool size max_connections is sized for. A malformed class is
# refused.

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
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
}

run "database" {
  command = plan

  assert {
    condition     = aws_db_instance.main.engine == "postgres" && aws_db_instance.main.engine_version == "17" && aws_db_instance.main.instance_class == "db.t4g.small"
    error_message = "PostgreSQL 17 on db.t4g.small"
  }
  assert {
    condition     = !aws_db_instance.main.publicly_accessible && aws_db_instance.main.multi_az
    error_message = "private and Multi-AZ (a standby RDS fails over to)"
  }
  assert {
    condition     = aws_db_instance.main.storage_encrypted && aws_db_instance.main.storage_type == "gp3" && aws_db_instance.main.allocated_storage == 20 && aws_db_instance.main.max_allocated_storage == 100
    error_message = "encrypted gp3, 20 GiB growing to 100"
  }
  assert {
    condition     = aws_db_instance.main.backup_retention_period == 14 && !aws_db_instance.main.delete_automated_backups && aws_db_instance.main.deletion_protection && !aws_db_instance.main.skip_final_snapshot
    error_message = "14 days of point-in-time recovery kept past a deletion, deletion protection and a final snapshot"
  }
  assert {
    condition     = aws_db_instance.main.password_wo_version == 1 && aws_db_instance.main.password == null
    error_message = "the master password is write-only (password_wo), never the stored password argument"
  }
  assert {
    condition     = aws_db_instance.main.auto_minor_version_upgrade && aws_db_instance.main.performance_insights_enabled
    error_message = "minor upgrades on, performance insights on"
  }
  assert {
    condition = (
      length([for p in aws_db_parameter_group.main.parameter : p if p.name == "rds.force_ssl" && p.value == "1"]) == 1 &&
      length([for p in aws_db_parameter_group.main.parameter : p if p.name == "log_min_duration_statement" && p.value == "1000"]) == 1 &&
      aws_db_parameter_group.main.family == "postgres17"
    )
    error_message = "the parameter group forces TLS and logs statements over a second"
  }
  assert {
    condition     = length([for p in aws_db_parameter_group.main.parameter : p if p.name == "max_connections" && p.value == "108" && p.apply_method == "pending-reboot"]) == 1
    error_message = "max_connections: 7 connections for each of 14 program processes, plus 10 (static: the next reboot)"
  }
  assert {
    condition     = toset(aws_db_subnet_group.main.subnet_ids) == toset([aws_subnet.private["a"].id, aws_subnet.private["b"].id])
    error_message = "the database lives in the private subnets"
  }
  assert {
    condition     = alltrue([for r in aws_vpc_security_group_ingress_rule.db_from_client : r.from_port == 5432 && r.to_port == 5432 && r.cidr_ipv4 == null])
    error_message = "the database admits port 5432 by security group, never by range"
  }
}

run "database_wiring" {
  command = plan

  assert {
    condition = (
      toset([for r in aws_vpc_security_group_ingress_rule.db_from_client : r.referenced_security_group_id]) == toset(["sg-0gate", "sg-0lobby", "sg-0web", "sg-0controller", "sg-0worker", "sg-0backup"]) &&
      alltrue([for r in aws_vpc_security_group_ingress_rule.db_from_client : r.security_group_id == aws_security_group.db.id])
    )
    error_message = "the database admits the five programs' security groups and the backup task's alone"
  }
  assert {
    condition = alltrue(concat(
      [for p, d in aws_ecs_task_definition.program : { for e in jsondecode(d.container_definitions)[0].environment : e.name => e.value }["DATABASE_URL"] == "postgresql://novaworld@opennova-prod-db.abcdefghijkl.us-east-1.rds.amazonaws.com:5432/novaworld?sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem"],
      [{ for e in jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].environment : e.name => e.value }["DATABASE_URL"] == "postgresql://novaworld@opennova-prod-db.abcdefghijkl.us-east-1.rds.amazonaws.com:5432/novaworld?sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem"],
      [for p, d in aws_ecs_task_definition.program : length([for s in jsondecode(d.container_definitions)[0].secrets : s if s.name == "DATABASE_PASSWORD"]) == 1],
    ))
    error_message = "every program's environment and the backup task's hand it a verify-full URL with no password in it; the password is a secret each reads"
  }
  assert {
    condition     = alltrue([for p, d in aws_ecs_task_definition.program : { for e in jsondecode(d.container_definitions)[0].environment : e.name => e.value }["DATABASE_POOL_MAX"] == "7"])
    error_message = "every program's environment names the pool size max_connections is sized for"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_DB_HOST='opennova-prod-db.abcdefghijkl.us-east-1.rds.amazonaws.com'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_DB_MASTER_USER='novaworld_admin'\n") &&
      !strcontains(aws_ssm_parameter.infra_env.value, "DATABASE_URL")
    )
    error_message = "infra.env hands the toolbox the database's names for its db commands"
  }
  assert {
    condition     = output.db_endpoint == "opennova-prod-db.abcdefghijkl.us-east-1.rds.amazonaws.com" && output.db_port == 5432
    error_message = "the outputs name the endpoint"
  }
}

# Single-AZ: the instance sits in private subnet a's zone.
run "single_az" {
  command = plan

  variables {
    db_multi_az = false
  }

  assert {
    condition     = !aws_db_instance.main.multi_az && aws_db_instance.main.availability_zone == aws_subnet.private["a"].availability_zone
    error_message = "single-AZ: in private subnet a's zone"
  }
}

# max_connections follows the pool and the process count.
run "connections_sized" {
  command = plan

  variables {
    db_pool_max          = 8
    db_service_processes = 22
  }

  assert {
    condition = (
      length([for p in aws_db_parameter_group.main.parameter : p if p.name == "max_connections" && p.value == "186"]) == 1 &&
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["DATABASE_POOL_MAX"] == "8"
    )
    error_message = "max_connections = db_pool_max x db_service_processes + 10, and the programs' environment names the pool"
  }
}

# max_connections must hold every program's tasks at its max (the worker's 8,
# doubled through a deployment, past 14).
run "processes_past_sizing_refused" {
  command = plan

  variables {
    worker = { max = 8 }
  }

  expect_failures = [aws_db_parameter_group.main]
}

# The programs' maxima alone (7 processes) fit 12, but a deployment's extra
# tasks (every program's doubled) make 14.
run "replacement_surge_counted" {
  command = plan

  variables {
    db_service_processes = 12
  }

  expect_failures = [aws_db_parameter_group.main]
}

# A worker holds a connection for each of its 4 job threads, its keeper and
# its main loop: a smaller pool runs fewer job threads than it asks for.
run "pool_too_small_refused" {
  command = plan

  variables {
    db_pool_max = 5
  }

  expect_failures = [var.db_pool_max]
}

run "malformed_class_refused" {
  command = plan

  variables {
    db_instance_class = "t4g.small"
  }

  expect_failures = [var.db_instance_class]
}

run "short_master_password_refused" {
  command = plan

  variables {
    db_master_password = "short"
  }

  expect_failures = [var.db_master_password]
}
