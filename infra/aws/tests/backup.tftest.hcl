# The database's nightly dump (backup.tf), planned against mock providers:
# offline, no credentials, nothing reaches AWS or Cloudflare. From the repo
# root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# A small Fargate task definition of the backup image, one dump a run, with
# the database's URL and the dumps' bucket and prefix in its environment and
# the database's password a secret; EventBridge Scheduler runs the release's
# revision of it (its image pinned by digest; off before the first release)
# nightly at 03:00 UTC on Fargate 1.4 in the public subnets, in its own
# security group (nothing inbound, the database admits it); the
# scheduler's role runs that family in this cluster alone and passes its two
# roles to ECS's tasks alone; the task's role writes and reads the
# deployment's dumps alone and reads the master password the db commands lend
# it, and ECS Exec's channels (an interactive psql).

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

run "backup" {
  command = plan

  assert {
    condition = (
      aws_ecs_task_definition.backup.family == "opennova-prod-backup" &&
      aws_ecs_task_definition.backup.cpu == "256" && aws_ecs_task_definition.backup.memory == "512" &&
      aws_ecs_task_definition.backup.requires_compatibilities == toset(["FARGATE"]) && aws_ecs_task_definition.backup.network_mode == "awsvpc" &&
      aws_ecs_task_definition.backup.task_role_arn == "arn:aws:iam::123456789012:role/opennova-prod-backup" &&
      aws_ecs_task_definition.backup.execution_role_arn == "arn:aws:iam::123456789012:role/opennova-prod-execution"
    )
    error_message = "a small Fargate task definition with its own task role"
  }
  assert {
    condition = (
      jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].name == "backup" &&
      jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].image == "ghcr.io/opennova-net/opennova-novaworld-backup:latest" &&
      jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].command == ["/backup/backup.sh"] &&
      jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].logConfiguration.options["awslogs-stream-prefix"] == "backup" &&
      { for e in jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].environment : e.name => e.value }["BACKUP_BUCKET"] == "opennova-prod-ops-123456789012" &&
      { for e in jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].environment : e.name => e.value }["BACKUP_PREFIX"] == "novaworld" &&
      jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].secrets == [{ name = "DATABASE_PASSWORD", valueFrom = "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/secret/DATABASE_PASSWORD" }]
    )
    error_message = "the backup image, one dump a run, into the operations bucket under novaworld/, its logs under backup/, the database's password a secret"
  }
  assert {
    condition = (
      aws_scheduler_schedule.backup.schedule_expression == "cron(0 3 * * ? *)" && aws_scheduler_schedule.backup.schedule_expression_timezone == "UTC" &&
      aws_scheduler_schedule.backup.flexible_time_window[0].mode == "OFF" &&
      aws_scheduler_schedule.backup.target[0].arn == "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod" &&
      aws_scheduler_schedule.backup.target[0].role_arn == "arn:aws:iam::123456789012:role/opennova-prod-backup-schedule" &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].task_definition_arn == "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup" &&
      aws_scheduler_schedule.backup.state == "DISABLED" &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].launch_type == "FARGATE" &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].platform_version == "1.4.0" &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].propagate_tags == "TASK_DEFINITION" &&
      toset(aws_scheduler_schedule.backup.target[0].ecs_parameters[0].network_configuration[0].subnets) == toset(["subnet-0public0a", "subnet-0public0b"]) &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].network_configuration[0].security_groups == toset(["sg-0backup"]) &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].network_configuration[0].assign_public_ip
    )
    error_message = "EventBridge Scheduler's nightly run at 03:00 UTC, on Fargate 1.4, in its own group, off before the first release (no backup image is pinned yet)"
  }
  assert {
    condition = (
      length([for st in data.aws_iam_policy_document.backup_schedule.statement : st if st.sid == "RunBackupTask" &&
        toset(st.actions) == toset(["ecs:RunTask"]) &&
        toset(st.resources) == toset(["arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup", "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup:*"]) &&
        length([for c in st.condition : c if c.variable == "ecs:cluster" && toset(c.values) == toset(["arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod"])]) == 1
      ]) == 1 &&
      length([for st in data.aws_iam_policy_document.backup_schedule.statement : st if st.sid == "PassBackupRoles" &&
        toset(st.resources) == toset(["arn:aws:iam::123456789012:role/opennova-prod-backup", "arn:aws:iam::123456789012:role/opennova-prod-execution"]) &&
        length([for c in st.condition : c if c.variable == "iam:PassedToService" && toset(c.values) == toset(["ecs-tasks.amazonaws.com"])]) == 1
      ]) == 1 &&
      toset(flatten([for st in data.aws_iam_policy_document.backup_schedule.statement : st.actions])) == toset(["ecs:RunTask", "iam:PassRole", "ecs:TagResource"]) &&
      length([for st in data.aws_iam_policy_document.scheduler_assume_role.statement : st if one(st.principals).identifiers == toset(["scheduler.amazonaws.com"])]) == 1
    )
    error_message = "the scheduler's role runs the backup family in this cluster alone and passes its two roles to ECS's tasks alone"
  }
  assert {
    condition = (
      toset(flatten([for st in data.aws_iam_policy_document.backup.statement : st.actions])) == toset(["s3:ListBucket", "s3:PutObject", "s3:GetObject", "s3:AbortMultipartUpload", "ssm:GetParameter"]) &&
      length([for st in data.aws_iam_policy_document.backup.statement : st if st.sid == "WriteAndReadDumps" && toset(st.resources) == toset(["arn:aws:s3:::opennova-prod-ops-123456789012/novaworld/*"])]) == 1 &&
      length([for st in data.aws_iam_policy_document.backup.statement : st if st.sid == "ReadLentMasterPassword" && toset(st.resources) == toset(["arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/bootstrap/db-master-password"])]) == 1 &&
      aws_iam_role_policy.backup.role == aws_iam_role.backup.id && aws_iam_role_policy.backup_exec.role == aws_iam_role.backup.id &&
      aws_iam_role.backup.permissions_boundary == "arn:aws:iam::123456789012:policy/opennova-role-boundary" &&
      aws_iam_role.backup_schedule.permissions_boundary == "arn:aws:iam::123456789012:policy/opennova-role-boundary"
    )
    error_message = "the backup task writes and reads the deployment's dumps alone (never deletes one) and reads the lent master password alone"
  }
  assert {
    condition = (
      aws_vpc_security_group_egress_rule.backup_all.security_group_id == "sg-0backup" &&
      length([for r in aws_vpc_security_group_ingress_rule.db_from_client : r if r.referenced_security_group_id == "sg-0backup"]) == 1
    )
    error_message = "the backup task's group admits nothing; the database admits it"
  }
}

# Released: the schedule is on and runs the release's backup revision (its
# image pinned by digest, as app deploy and app rollback point it), never the
# family's newest revision, which an apply's template (`latest`) may be. A
# manifest naming another family's revision is not followed.
run "backup_released" {
  command = plan

  override_data {
    target          = data.aws_ssm_parameters_by_path.deployment
    override_during = plan
    values = {
      names  = ["/opennova/prod/release"]
      values = ["{\"id\":\"20261016T000000Z-abc1234\",\"programs\":[\"worker\",\"controller\",\"web\",\"lobby\",\"gate\"],\"task_definitions\":{\"backup\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup:7\"}}"]
    }
  }

  assert {
    condition = (
      aws_scheduler_schedule.backup.state == "ENABLED" &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].task_definition_arn == "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup:7"
    )
    error_message = "released, the nightly run is on and runs the release's backup revision"
  }
  assert {
    condition     = strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_BACKUP_SCHEDULE='opennova-prod-backup'\n")
    error_message = "infra.env names the schedule the toolbox points at each release's backup revision"
  }
}

run "backup_released_foreign_revision" {
  command = plan

  override_data {
    target          = data.aws_ssm_parameters_by_path.deployment
    override_during = plan
    values = {
      names  = ["/opennova/prod/release"]
      values = ["{\"id\":\"20261016T000000Z-abc1234\",\"programs\":[\"gate\"],\"task_definitions\":{\"backup\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/elsewhere:3\"}}"]
    }
  }

  assert {
    condition     = aws_scheduler_schedule.backup.target[0].ecs_parameters[0].task_definition_arn == "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup"
    error_message = "a manifest's revision of another family is not followed: the schedule names its own family"
  }
}
