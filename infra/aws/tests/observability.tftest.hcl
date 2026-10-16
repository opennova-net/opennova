# Observability and budgets (observability.tf), planned against mock providers
# whose values reach the plan: offline, no credentials, nothing reaches AWS or
# Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# The service's and machines' log groups keep 30 days; the server's heartbeat
# line becomes metrics; its silence for 5 minutes is an alarm (no data
# breaches), as are a crash loop, failed servers, lost machines, failed
# launches, a stale billing sync, refused webhooks, entitlement stops held for
# 5 minutes, the service instance's status checks (recover, reboot) and CPU, the database's credits and storage and the
# /healthz health check; every alarm notifies the one topic, which emails the
# alert address. Prod makes the cost and egress budgets and the anomaly
# monitor; a placeholder address subscribes nothing.

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

override_resource {
  target          = aws_route53_health_check.service
  override_during = plan
  values          = { id = "hc-0service" }
}

variables {
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
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
      aws_cloudwatch_log_metric_filter.heartbeat.pattern == "{ $.heartbeat.nw_sessions >= 0 }" &&
      aws_cloudwatch_log_metric_filter.heartbeat.metric_transformation[0].name == "Heartbeat" &&
      aws_cloudwatch_log_metric_filter.heartbeat.metric_transformation[0].namespace == "OpenNova/prod" &&
      aws_cloudwatch_log_metric_filter.heartbeat.metric_transformation[0].value == "1"
    )
    error_message = "each heartbeat line counts once"
  }
  assert {
    condition = (
      length(aws_cloudwatch_log_metric_filter.heartbeat_field) == 9 &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["LimitsHeld"].metric_transformation[0].value == "$.heartbeat.limits_held" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["LaunchFailures"].metric_transformation[0].value == "$.heartbeat.launch_failures" &&
      aws_cloudwatch_log_metric_filter.heartbeat_field["BillingSyncAgeS"].pattern == "{ $.heartbeat.billing_sync_age_s >= 0 }"
    )
    error_message = "every heartbeat field is a metric"
  }
  assert {
    condition     = aws_cloudwatch_log_metric_filter.boots.pattern == "\"[boot] migrations:\""
    error_message = "each boot's migrations line counts a boot"
  }
  assert {
    condition = (
      aws_cloudwatch_metric_alarm.server["heartbeat-missing"].treat_missing_data == "breaching" &&
      aws_cloudwatch_metric_alarm.server["heartbeat-missing"].comparison_operator == "LessThanThreshold" &&
      aws_cloudwatch_metric_alarm.server["heartbeat-missing"].threshold == 1 &&
      aws_cloudwatch_metric_alarm.server["heartbeat-missing"].period == 300
    )
    error_message = "a heartbeat missing for 5 minutes is an alarm, no data included"
  }
  assert {
    condition = (
      aws_cloudwatch_metric_alarm.server["crash-loop"].metric_name == "Boots" && aws_cloudwatch_metric_alarm.server["crash-loop"].threshold == 3 &&
      aws_cloudwatch_metric_alarm.server["crash-loop"].comparison_operator == "GreaterThanThreshold" && aws_cloudwatch_metric_alarm.server["crash-loop"].period == 900 &&
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
      contains(aws_cloudwatch_metric_alarm.host_recover.alarm_actions, "arn:aws:automate:us-east-1:ec2:recover") &&
      contains(aws_cloudwatch_metric_alarm.host_reboot.alarm_actions, "arn:aws:automate:us-east-1:ec2:reboot") &&
      aws_cloudwatch_metric_alarm.host_cpu.threshold == 80
    )
    error_message = "the host's status checks recover and reboot it; its CPU over 80% alarms"
  }
  assert {
    condition     = aws_cloudwatch_metric_alarm.db_cpu_credits.namespace == "AWS/RDS" && aws_cloudwatch_metric_alarm.db_free_storage.threshold == 2147483648
    error_message = "the database's credits and free storage alarm"
  }
  assert {
    condition = (
      aws_route53_health_check.service.ip_address == "54.162.79.147" && aws_route53_health_check.service.port == 8080 &&
      aws_route53_health_check.service.resource_path == "/healthz" && aws_route53_health_check.service.type == "HTTP" &&
      length(aws_cloudwatch_metric_alarm.healthz) == 1
    )
    error_message = "Route 53 checks http://<Elastic IP>:8080/healthz, and its failure alarms"
  }
  assert {
    condition = alltrue(concat(
      [for a in aws_cloudwatch_metric_alarm.server : contains(a.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-server-alerts")],
      [contains(aws_cloudwatch_metric_alarm.host_cpu.alarm_actions, "arn:aws:sns:us-east-1:123456789012:opennova-server-alerts")],
    ))
    error_message = "every alarm notifies the alerts topic"
  }
  assert {
    condition     = length(aws_sns_topic_subscription.alerts_email) == 1 && aws_sns_topic_subscription.alerts_email[0].endpoint == "ops@example.com"
    error_message = "the topic emails the alert address"
  }
  assert {
    condition = (
      length(aws_budgets_budget.monthly_cost) == 1 && length(aws_budgets_budget.monthly_cost[0].notification) == 2 &&
      length(aws_budgets_budget.egress) == 1 && aws_budgets_budget.egress[0].budget_type == "USAGE" &&
      length([for f in aws_budgets_budget.egress[0].cost_filter : f if f.name == "UsageTypeGroup" && contains(f.values, "EC2: Data Transfer - Internet (Out)")]) == 1
    )
    error_message = "prod makes the monthly cost budget and the EC2 egress budget, each emailing at 80% spent and a 100% forecast"
  }
  assert {
    condition = alltrue([for b in [aws_budgets_budget.monthly_cost[0], aws_budgets_budget.egress[0]] :
      length([for f in b.cost_filter : f if f.name == "TagKeyValue" && contains(f.values, "user:opennova:deployment$opennova-server") && contains(f.values, "user:opennova:deployment$opennova-server-staging")]) == 1
    ])
    error_message = "both budgets count only OpenNova's deployments (their tag), prod's and staging's, in an account other projects share"
  }
  assert {
    condition     = length(aws_ce_anomaly_monitor.services) == 1 && length(aws_ce_anomaly_subscription.email) == 1
    error_message = "prod makes the cost anomaly monitor and its email"
  }
  assert {
    condition = (
      aws_ce_anomaly_monitor.services[0].monitor_type == "CUSTOM" &&
      jsondecode(aws_ce_anomaly_monitor.services[0].monitor_specification).Tags.Key == "user:opennova:deployment" &&
      contains(jsondecode(aws_ce_anomaly_monitor.services[0].monitor_specification).Tags.Values, "opennova-server")
    )
    error_message = "the cost anomaly monitor watches OpenNova's deployments' tag alone"
  }
}

run "placeholder_email" {
  command = plan

  variables {
    alert_email = "REPLACE_ME"
  }

  assert {
    condition     = length(aws_sns_topic_subscription.alerts_email) == 0 && length(aws_ce_anomaly_subscription.email) == 0 && length(aws_budgets_budget.monthly_cost[0].notification) == 0
    error_message = "a placeholder address subscribes nothing"
  }
  assert {
    condition     = length(aws_cloudwatch_metric_alarm.server) == 8 && length(aws_ce_anomaly_monitor.services) == 1
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
