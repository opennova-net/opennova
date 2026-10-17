# Observability and budgets (observability.tf), planned against mock providers
# whose values reach the plan: offline, no credentials, nothing reaches AWS or
# Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# The service's and machines' log groups keep 30 days; each process's
# heartbeat line becomes metrics, by program; a program's silence for 5 minutes
# is an alarm (no data breaches once the programs have a release, and is none
# before it, when no task runs), as are a program's crash loop, its running
# tasks under its min, its CPU and memory, an unhealthy target behind the load
# balancer, failed servers, lost machines, failed launches, a stale billing
# sync, refused webhooks, entitlement stops held for 5 minutes, a gate probe
# left unanswered for want of a lobby, the database's
# CPU, CPU credits (a burstable class), connections and storage and the
# /healthz health check at the gate's address; every alarm notifies the one
# topic, which emails the alert address. Each deployment makes its account's
# cost and egress budgets and anomaly monitor (staging's own, with its own
# limits); a placeholder address subscribes nothing.

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
  target          = aws_route53_health_check.service
  override_during = plan
  values          = { id = "hc-0service" }
}

variables {
  deployment                = "prod"
  aws_account_id            = "123456789012"
  region                    = "us-east-1"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
  alert_email               = "ops@example.com"
}

run "observability" {
  command = plan

  assert {
    condition = (
      aws_cloudwatch_log_group.service.name == "/opennova/prod/service" && aws_cloudwatch_log_group.service.retention_in_days == 30 &&
      aws_cloudwatch_log_group.machines.name == "/opennova/prod/machines" && aws_cloudwatch_log_group.machines.retention_in_days == 30
    )
    error_message = "the service's and the machines' logs, 30 days each"
  }
  assert {
    condition = (
      toset(keys(aws_cloudwatch_log_metric_filter.heartbeat)) == toset(["gate", "lobby", "web", "controller", "worker"]) &&
      aws_cloudwatch_log_metric_filter.heartbeat["lobby"].pattern == "{ $.heartbeat.program = \"lobby\" }" &&
      aws_cloudwatch_log_metric_filter.heartbeat["lobby"].metric_transformation[0].name == "Heartbeat" &&
      aws_cloudwatch_log_metric_filter.heartbeat["lobby"].metric_transformation[0].namespace == "OpenNova/prod" &&
      aws_cloudwatch_log_metric_filter.heartbeat["lobby"].metric_transformation[0].value == "1" &&
      aws_cloudwatch_log_metric_filter.heartbeat["lobby"].metric_transformation[0].dimensions == tomap({ Program = "$.heartbeat.program" })
    )
    error_message = "each heartbeat line counts once, by its program"
  }
  assert {
    condition = (
      length(aws_cloudwatch_log_metric_filter.heartbeat_field) == 11 &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["LimitsHeld"].metric_transformation[0].value == "$.heartbeat.limits_held" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["ProbesUnanswered"].pattern == "{ $.heartbeat.probes_unanswered >= 0 }" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["ProbesUnanswered"].metric_transformation[0].value == "$.heartbeat.probes_unanswered" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["LobbiesLive"].pattern == "{ $.heartbeat.lobbies_live >= 0 }" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["LobbiesLive"].metric_transformation[0].value == "$.heartbeat.lobbies_live" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["LaunchFailures"].metric_transformation[0].value == "$.heartbeat.launch_failures" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["BillingSyncAgeS"].pattern == "{ $.heartbeat.billing_sync_age_s >= 0 }"
    )
    error_message = "every heartbeat field is a metric"
  }
  assert {
    condition = (
      aws_cloudwatch_log_metric_filter.boots["worker"].pattern == "{ $.boot.program = \"worker\" }" &&
      aws_cloudwatch_log_metric_filter.boots["worker"].metric_transformation[0].name == "Boots" &&
      aws_cloudwatch_log_metric_filter.boots["worker"].metric_transformation[0].dimensions == tomap({ Program = "$.boot.program" }) &&
      length(aws_cloudwatch_log_metric_filter.boots) == 5
    )
    error_message = "each process's boot line counts a boot of its program"
  }
  assert {
    condition = alltrue([
      for program, a in aws_cloudwatch_metric_alarm.program_heartbeat :
      a.treat_missing_data == "notBreaching" && a.comparison_operator == "LessThanThreshold" && a.threshold == 1 &&
      a.period == 300 && a.metric_name == "Heartbeat" && a.dimensions == tomap({ Program = program })
    ]) && length(aws_cloudwatch_metric_alarm.program_heartbeat) == 5
    error_message = "a program's heartbeat missing for 5 minutes is an alarm; before the first release (no task runs) no data is none"
  }
  assert {
    condition = (
      aws_cloudwatch_metric_alarm.program_crash_loop["lobby"].threshold == 4 && aws_cloudwatch_metric_alarm.program_crash_loop["web"].threshold == 4 &&
      aws_cloudwatch_metric_alarm.program_crash_loop["worker"].metric_name == "Boots" && aws_cloudwatch_metric_alarm.program_crash_loop["worker"].dimensions == tomap({ Program = "worker" }) &&
      aws_cloudwatch_metric_alarm.program_crash_loop["worker"].comparison_operator == "GreaterThanThreshold" && aws_cloudwatch_metric_alarm.program_crash_loop["worker"].period == 900
    )
    error_message = "a program's crash loop: more boots in 15 minutes than its max and 3"
  }
  assert {
    condition = (
      aws_cloudwatch_metric_alarm.server["launch-failures"].threshold == 3 && aws_cloudwatch_metric_alarm.server["launch-failures"].statistic == "Sum" &&
      aws_cloudwatch_metric_alarm.server["webhook-rejects"].threshold == 5 &&
      aws_cloudwatch_metric_alarm.server["billing-sync-stale"].threshold == 3600 &&
      aws_cloudwatch_metric_alarm.server["hosted-failed"].threshold == 0 && aws_cloudwatch_metric_alarm.server["machines-lost"].threshold == 0
    )
    error_message = "the crash loop, launch failure, webhook, billing sync, failed server and lost machine alarms"
  }
  assert {
    condition = (
      aws_cloudwatch_metric_alarm.server["limits-held"].metric_name == "LimitsHeld" &&
      aws_cloudwatch_metric_alarm.server["limits-held"].statistic == "Minimum" &&
      aws_cloudwatch_metric_alarm.server["limits-held"].period == 300 && aws_cloudwatch_metric_alarm.server["limits-held"].evaluation_periods == 1 &&
      aws_cloudwatch_metric_alarm.server["limits-held"].comparison_operator == "GreaterThanOrEqualToThreshold" &&
      aws_cloudwatch_metric_alarm.server["limits-held"].threshold == 1 &&
      aws_cloudwatch_metric_alarm.server["limits-held"].treat_missing_data == "notBreaching"
    )
    error_message = "limits_held at 1 in every heartbeat for 5 minutes is an alarm; no data is none"
  }
  assert {
    condition = (
      aws_cloudwatch_metric_alarm.server["probes-unanswered"].metric_name == "ProbesUnanswered" &&
      aws_cloudwatch_metric_alarm.server["probes-unanswered"].statistic == "Sum" &&
      aws_cloudwatch_metric_alarm.server["probes-unanswered"].period == 300 && aws_cloudwatch_metric_alarm.server["probes-unanswered"].evaluation_periods == 1 &&
      aws_cloudwatch_metric_alarm.server["probes-unanswered"].comparison_operator == "GreaterThanThreshold" &&
      aws_cloudwatch_metric_alarm.server["probes-unanswered"].threshold == 0 &&
      aws_cloudwatch_metric_alarm.server["probes-unanswered"].treat_missing_data == "notBreaching"
    )
    error_message = "a gate probe left unanswered for want of a lobby in 5 minutes (Sum > 0) is an alarm; no data (no gate) is the heartbeat alarm's"
  }
  assert {
    condition = (
      length(aws_cloudwatch_metric_alarm.program_utilization) == 10 &&
      aws_cloudwatch_metric_alarm.program_utilization["gate-cpu"].namespace == "AWS/ECS" &&
      aws_cloudwatch_metric_alarm.program_utilization["gate-cpu"].metric_name == "CPUUtilization" &&
      aws_cloudwatch_metric_alarm.program_utilization["worker-memory"].metric_name == "MemoryUtilization" &&
      aws_cloudwatch_metric_alarm.program_utilization["worker-memory"].dimensions == tomap({ ClusterName = "opennova-prod", ServiceName = "worker" }) &&
      alltrue([for a in aws_cloudwatch_metric_alarm.program_utilization : a.threshold == 80 && a.period == 300 && a.evaluation_periods == 3])
    )
    error_message = "each program's CPU and memory over 80% for 15 minutes alarms"
  }
  assert {
    condition = (
      length(aws_cloudwatch_metric_alarm.program_running) == 5 &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].namespace == "AWS/ECS" &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].metric_name == "CPUUtilization" &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].statistic == "SampleCount" &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].dimensions == tomap({ ClusterName = "opennova-prod", ServiceName = "lobby" }) &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].comparison_operator == "LessThanThreshold" &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].threshold == 0 &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].period == 60 && aws_cloudwatch_metric_alarm.program_running["lobby"].evaluation_periods == 5 &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].treat_missing_data == "notBreaching"
    )
    error_message = "a program running fewer tasks than its min for 5 minutes alarms (one CPU sample a running task a minute), its floor 0 and no data none until the first release"
  }
  assert {
    condition = (
      toset(keys(aws_cloudwatch_metric_alarm.lb_unhealthy)) == toset(["gate", "lobby", "http", "update"]) &&
      aws_cloudwatch_metric_alarm.lb_unhealthy["gate"].namespace == "AWS/NetworkELB" &&
      aws_cloudwatch_metric_alarm.lb_unhealthy["gate"].metric_name == "UnHealthyHostCount" &&
      aws_cloudwatch_metric_alarm.lb_unhealthy["gate"].dimensions == tomap({ LoadBalancer = "net/opennova-prod/0123456789abcdef", TargetGroup = "targetgroup/opennova-prod-gate/0123456789abcdef" })
    )
    error_message = "a gate, lobby or web task out of the load balancer for 10 minutes alarms, per target group"
  }
  assert {
    condition = (
      aws_cloudwatch_metric_alarm.db_cpu.namespace == "AWS/RDS" && aws_cloudwatch_metric_alarm.db_cpu.threshold == 80 &&
      aws_cloudwatch_metric_alarm.db_connections.metric_name == "DatabaseConnections" && aws_cloudwatch_metric_alarm.db_connections.threshold == 64 &&
      aws_cloudwatch_metric_alarm.db_free_storage.threshold == 2147483648
    )
    error_message = "the database's CPU, its connections at 80% of max_connections (80), and its free storage alarm"
  }
  assert {
    condition = (
      aws_route53_health_check.service.ip_address == "54.162.79.147" && aws_route53_health_check.service.port == 8080 &&
      aws_route53_health_check.service.resource_path == "/healthz" && aws_route53_health_check.service.type == "HTTP" &&
      length(aws_cloudwatch_metric_alarm.healthz) == 1
    )
    error_message = "Route 53 checks http://<the gate's address>:8080/healthz, and its failure alarms"
  }
  assert {
    condition = alltrue(concat(
      [for a in aws_cloudwatch_metric_alarm.server : contains(a.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts")],
      [for a in aws_cloudwatch_metric_alarm.program_heartbeat : contains(a.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts")],
      [for a in aws_cloudwatch_metric_alarm.program_crash_loop : contains(a.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts")],
      [for a in aws_cloudwatch_metric_alarm.program_running : contains(a.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts")],
      [for a in aws_cloudwatch_metric_alarm.program_utilization : contains(a.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts")],
      [for a in aws_cloudwatch_metric_alarm.lb_unhealthy : contains(a.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts")],
    ))
    error_message = "every alarm notifies the alerts topic"
  }
  assert {
    condition     = length(aws_sns_topic_subscription.alerts_email) == 1 && aws_sns_topic_subscription.alerts_email[0].endpoint == "ops@example.com"
    error_message = "the topic emails the alert address"
  }
  assert {
    condition = (
      aws_budgets_budget.monthly_cost.name == "opennova-prod-monthly-cost" && length(aws_budgets_budget.monthly_cost.notification) == 2 &&
      aws_budgets_budget.egress.name == "opennova-prod-ec2-egress" && aws_budgets_budget.egress.budget_type == "USAGE" &&
      length([for f in aws_budgets_budget.egress.cost_filter : f if f.name == "UsageTypeGroup" && contains(f.values, "EC2: Data Transfer - Internet (Out)")]) == 1
    )
    error_message = "prod makes its own monthly cost budget and EC2 egress budget, each emailing at 80% spent and a 100% forecast"
  }
  assert {
    condition = alltrue([for b in [aws_budgets_budget.monthly_cost, aws_budgets_budget.egress] :
      length([for f in b.cost_filter : f if f.name == "TagKeyValue"]) == 0
    ])
    error_message = "the budgets count the deployment's whole account (it holds the deployment alone): no tag filter, no other deployment's costs"
  }
  assert {
    condition = (
      length(aws_ce_anomaly_monitor.services) == 1 && length(aws_ce_anomaly_subscription.email) == 1 &&
      aws_ce_anomaly_monitor.services[0].name == "opennova-prod-services" &&
      aws_ce_anomaly_monitor.services[0].monitor_type == "DIMENSIONAL" && aws_ce_anomaly_monitor.services[0].monitor_dimension == "SERVICE"
    )
    error_message = "prod's account's cost anomaly monitor (by AWS service) and its email"
  }
}

# Once the programs have a release (app deploy's manifest), a program's silence
# and its tasks missing are alarms: no data breaches, the running floor is its
# min.
run "released" {
  command = plan

  override_data {
    target          = data.aws_ssm_parameters_by_path.deployment
    override_during = plan
    values = {
      names  = ["/opennova/prod/release"]
      values = ["{\"id\":\"20261016T000000Z-abc1234\",\"programs\":[\"worker\",\"controller\",\"web\",\"lobby\",\"gate\"]}"]
    }
  }

  assert {
    condition = alltrue(concat(
      [for a in aws_cloudwatch_metric_alarm.program_heartbeat : a.treat_missing_data == "breaching"],
      [for a in aws_cloudwatch_metric_alarm.program_running : a.treat_missing_data == "breaching"],
    )) && length(aws_cloudwatch_metric_alarm.program_heartbeat) == 5 && length(aws_cloudwatch_metric_alarm.program_running) == 5
    error_message = "released, a program's missing heartbeat and its missing tasks are alarms, no data included"
  }
  assert {
    condition     = aws_cloudwatch_metric_alarm.program_running["lobby"].threshold == 1 && aws_cloudwatch_metric_alarm.program_running["web"].threshold == 1
    error_message = "released, the running floor is each program's min"
  }
}

run "placeholder_email" {
  command = plan

  variables {
    alert_email = "REPLACE_ME"
  }

  assert {
    condition     = length(aws_sns_topic_subscription.alerts_email) == 0 && length(aws_ce_anomaly_subscription.email) == 0 && length(aws_budgets_budget.monthly_cost.notification) == 0
    error_message = "a placeholder address subscribes nothing"
  }
  assert {
    condition     = length(aws_cloudwatch_metric_alarm.server) == 7 && length(aws_cloudwatch_metric_alarm.program_heartbeat) == 5 && length(aws_ce_anomaly_monitor.services) == 1
    error_message = "the alarms and the monitor exist all the same"
  }
}

run "other_region" {
  command = plan

  variables {
    region = "us-west-2"
  }

  assert {
    condition     = length(aws_cloudwatch_metric_alarm.healthz) == 0
    error_message = "Route 53's metrics live in us-east-1 alone: no health check alarm elsewhere"
  }
}

# A burstable database (db.t4g.small, the default) runs in RDS's unlimited
# mode: a low CPU credit balance is an alarm (its load past the baseline is
# billed). Another class has no credits.
run "db_cpu_credits" {
  command = plan

  assert {
    condition = (
      length(aws_cloudwatch_metric_alarm.db_cpu_credits) == 1 &&
      aws_cloudwatch_metric_alarm.db_cpu_credits[0].namespace == "AWS/RDS" &&
      aws_cloudwatch_metric_alarm.db_cpu_credits[0].metric_name == "CPUCreditBalance" &&
      aws_cloudwatch_metric_alarm.db_cpu_credits[0].statistic == "Minimum" &&
      aws_cloudwatch_metric_alarm.db_cpu_credits[0].comparison_operator == "LessThanThreshold" &&
      aws_cloudwatch_metric_alarm.db_cpu_credits[0].threshold == 50 &&
      contains(aws_cloudwatch_metric_alarm.db_cpu_credits[0].alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts")
    )
    error_message = "a burstable database's CPU credit balance under 50 for 15 minutes alarms"
  }
}

run "db_cpu_credits_burstable_only" {
  command = plan

  variables {
    db_instance_class = "db.m7g.large"
  }

  assert {
    condition     = length(aws_cloudwatch_metric_alarm.db_cpu_credits) == 0 && aws_cloudwatch_metric_alarm.db_cpu.threshold == 80
    error_message = "a class with no credits has no credit alarm, its CPU alarm all the same"
  }
}

# Staging's account: its own budgets, by its own limits, under its own names.
run "staging_budgets" {
  command = plan

  variables {
    deployment                = "staging"
    service_eip_allocation_id = ""
    budget_monthly_usd        = 250
    budget_egress_gb          = 200
  }

  assert {
    condition = (
      aws_budgets_budget.monthly_cost.name == "opennova-staging-monthly-cost" && aws_budgets_budget.monthly_cost.limit_amount == "250" &&
      aws_budgets_budget.egress.name == "opennova-staging-ec2-egress" && aws_budgets_budget.egress.limit_amount == "200" &&
      aws_ce_anomaly_monitor.services[0].name == "opennova-staging-services"
    )
    error_message = "staging's account has budgets and an anomaly monitor of its own"
  }
  assert {
    condition     = aws_sns_topic.alerts.name == "opennova-staging-alerts" && aws_cloudwatch_log_group.service.name == "/opennova/staging/service"
    error_message = "staging's alarms go to its own topic, its logs to its own group"
  }
}
