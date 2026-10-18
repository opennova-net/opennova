# Staging (service.tf: deployment = "staging", in its own account), planned
# against mock providers: offline, no credentials, nothing reaches AWS or
# Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Staging gets an Elastic IP of its own, managed here, which its load balancer
# takes in zone a as prod's takes prod's, and which its programs announce;
# its records, its cluster, its services, its Cloud Map name, its cache, its
# operations bucket (the game data's store among it, with hosting on) and its
# backups are its own, every name carrying `staging`. It never adopts an
# address: an allocation id (a stray var-file holding prod's) stops the plan at
# the adopted address's precondition, before the address is read.

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
    defaults = { id = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-staging", arn = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-staging" }
  }
  mock_resource "aws_ecs_task_definition" {
    defaults = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/mock:1" }
  }
  mock_resource "aws_ecs_service" {
    defaults = { id = "arn:aws:ecs:us-east-1:123456789012:service/opennova-staging/mock" }
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
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-staging-gate/0123456789abcdef", arn_suffix = "targetgroup/opennova-staging-gate/0123456789abcdef" }
}
override_resource {
  target          = aws_lb_target_group.public["https"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-staging-https/0123456789abcdef", arn_suffix = "targetgroup/opennova-staging-https/0123456789abcdef" }
}
override_resource {
  target          = aws_lb_target_group.public["http"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-staging-http/0123456789abcdef", arn_suffix = "targetgroup/opennova-staging-http/0123456789abcdef" }
}
override_resource {
  target          = aws_eip.lb_b
  override_during = plan
  values          = { allocation_id = "eipalloc-0000000000000000b", public_ip = "5.5.5.5" }
}
override_resource {
  target          = aws_ecs_task_definition.program["gate"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-gate:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["lobby"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-lobby:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["web"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-web:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-fleet:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-worker:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-data:1" }
}
override_resource {
  target          = aws_ecs_task_definition.backup
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-backup:1" }
}
override_resource {
  target          = aws_iam_role.program["gate"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-gate" }
}
override_resource {
  target          = aws_iam_role.program["lobby"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-lobby" }
}
override_resource {
  target          = aws_iam_role.program["web"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-web" }
}
override_resource {
  target          = aws_iam_role.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-fleet" }
}
override_resource {
  target          = aws_iam_role.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-worker" }
}
override_resource {
  target          = aws_iam_role.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-data" }
}
override_resource {
  target          = aws_iam_role.execution
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-execution" }
}
override_resource {
  target          = aws_iam_role.backup
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-backup" }
}
override_resource {
  target          = aws_iam_role.backup_schedule
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-staging-backup-schedule" }
}

variables {
  deployment         = "staging"
  aws_account_id     = "123456789012"
  region             = "us-east-1"
  db_master_password = "0123456789abcdef0123456789abcdef"
  cache_auth_token   = "fedcba9876543210fedcba9876543210"
  cloudflare_zone_id = "0123456789abcdef0123456789abcdef"
  cloudflare_domain  = "example.com"
}

run "own_address" {
  command = plan

  assert {
    condition     = length(data.aws_eip.gate) == 0 && length(aws_eip.gate) == 1 && length(aws_ec2_tag.gate_eip) == 0
    error_message = "staging adopts no address: it gets one of its own"
  }
  assert {
    condition = (
      toset([for m in aws_lb.public.subnet_mapping : "${m.subnet_id}=${m.allocation_id}"]) == toset([
        "subnet-0public0a=eipalloc-0fedcba9876543210",
        "subnet-0public0b=eipalloc-0000000000000000b",
      ]) && aws_lb.public.name == "opennova-staging"
    )
    error_message = "staging's load balancer takes its own address in zone a"
  }
  assert {
    condition = (
      output.public_ip == "3.3.3.3" &&
      { for e in jsondecode(aws_ecs_task_definition.program["gate"].container_definitions)[0].environment : e.name => e.value }["ONNET_PUBLIC_HOST"] == "3.3.3.3" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_WEB_HOST"] == "3.3.3.3" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_SITE_URL"] == "https://staging.example.com" &&
      output.internal_url == "http://fleet.staging.opennova.internal:8070"
    )
    error_message = "staging's programs announce staging's own address and its site, and its fleet program is named in staging's own zone"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_DEPLOYMENT='staging'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_CLUSTER='opennova-staging'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "BACKUP_PREFIX='novaworld'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_OPS_BUCKET='opennova-staging-ops-123456789012'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "BACKUP_BUCKET='opennova-staging-ops-123456789012'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_ACCOUNT_ID='123456789012'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_PUBLIC_HOST='3.3.3.3'\n") &&
      aws_ssm_parameter.infra_env.name == "/opennova/staging/infra.env"
    )
    error_message = "infra.env (under /opennova/staging/) names staging's account, cluster, bucket, backup prefix and address"
  }
  assert {
    condition = (
      cloudflare_record.web_root[0].name == "staging" && cloudflare_record.web_root[0].type == "CNAME" &&
      cloudflare_record.web_root[0].content == "opennova-prod-0123456789abcdef.elb.us-east-1.amazonaws.com" &&
      length(cloudflare_record.web_www) == 0
    )
    error_message = "staging's record (staging; no www) points at its own load balancer"
  }
  assert {
    condition     = !strcontains(aws_ecs_task_definition.program["web"].container_definitions, "ONNET_OBJECT_STORE")
    error_message = "hosting off, no store"
  }
  assert {
    condition = (
      aws_ecs_cluster.main.name == "opennova-staging" &&
      alltrue([for p, d in aws_ecs_task_definition.program : d.family == "opennova-staging-${p}"]) &&
      aws_service_discovery_private_dns_namespace.internal.name == "staging.opennova.internal" &&
      aws_appautoscaling_target.program["web"].resource_id == "service/opennova-staging/web"
    )
    error_message = "staging's cluster, task definitions, services and Cloud Map namespace are its own"
  }
  assert {
    condition = (
      aws_elasticache_replication_group.cache.replication_group_id == "opennova-staging-cache" &&
      startswith({ for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_REDIS_URL"], "rediss://")
    )
    error_message = "staging has a cache of its own, named in web's environment"
  }
  assert {
    condition = (
      aws_scheduler_schedule.backup.name == "opennova-staging-backup" &&
      aws_scheduler_schedule.backup.target[0].ecs_parameters[0].task_definition_arn == "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-staging-backup" &&
      { for e in jsondecode(aws_ecs_task_definition.backup.container_definitions)[0].environment : e.name => e.value }["BACKUP_BUCKET"] == "opennova-staging-ops-123456789012" &&
      aws_s3_bucket.ops.bucket == "opennova-staging-ops-123456789012" &&
      length([for st in data.aws_iam_policy_document.backup.statement : st if st.sid == "WriteAndReadDumps" && toset(st.resources) == toset(["arn:aws:s3:::opennova-staging-ops-123456789012/novaworld/*"])]) == 1
    )
    error_message = "staging's nightly dump runs its own backup task into staging's own operations bucket, which its task role alone writes"
  }
  assert {
    condition = (
      alltrue([for p, r in aws_iam_role.program : r.name == "opennova-staging-${p}"]) &&
      aws_iam_role.execution.name == "opennova-staging-execution" && aws_iam_role.backup.name == "opennova-staging-backup" &&
      aws_db_instance.main.identifier == "opennova-staging-db" && aws_sns_topic.alerts.name == "opennova-staging-alerts" &&
      aws_cloudwatch_log_group.service.name == "/opennova/staging/service" &&
      alltrue([for p, g in aws_security_group.program : g.name == "opennova-staging-${p}"])
    )
    error_message = "every name staging makes carries staging: its roles, database, topic, logs and groups"
  }
  assert {
    condition = length([for st in data.aws_iam_policy_document.execution.statement : st if st.sid == "ReadTaskSecrets" && toset(st.resources) == toset([
      "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/staging/secret/*", "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/staging/tls/*",
    ])]) == 1
    error_message = "staging's tasks start with staging's secrets alone"
  }
}

run "adopted_refused" {
  command = plan

  variables {
    service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  }

  expect_failures = [data.aws_eip.gate]
}

# Hosting on: the store is staging's own bucket under data/, and the roles
# narrow to it.
run "hosting_on" {
  command = plan

  override_resource {
    target          = aws_security_group.machines[0]
    override_during = plan
    values          = { id = "sg-0machines" }
  }
  override_resource {
    target          = aws_launch_template.fleet["shared"]
    override_during = plan
    values          = { id = "lt-0shared", arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0shared" }
  }
  override_resource {
    target          = aws_launch_template.fleet["own"]
    override_during = plan
    values          = { id = "lt-0own", arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0own" }
  }

  variables {
    hosted_enabled = true
    fleet_ami      = "al2023-ami-ecs-hvm-2023.0.20261009-kernel-6.1-x86_64"
  }

  assert {
    condition = alltrue([
      for p in ["web", "fleet", "worker", "data"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_OBJECT_STORE_URL"] == "s3://opennova-staging-ops-123456789012/data"
    ])
    error_message = "staging's programs name staging's own bucket under data/"
  }
  assert {
    condition = (
      length([
        for st in data.aws_iam_policy_document.data_store[0].statement : st
        if st.sid == "DataStoreObjects" && toset(st.resources) == toset(["arn:aws:s3:::opennova-staging-ops-123456789012/data/*"])
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.hostd[0].statement : st
        if st.sid == "ReadDataTrees" && toset(st.resources) == toset(["arn:aws:s3:::opennova-staging-ops-123456789012/data/trees/*"])
      ]) == 1 &&
      alltrue([for p, r in aws_iam_role_policy.data_store : r.name == "opennova-staging-${p}-data-store"])
    )
    error_message = "web's, the worker's and hostd's store access is staging's bucket alone"
  }
}
