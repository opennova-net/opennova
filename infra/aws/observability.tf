# ---------------------------------------------------------------------------
# Observability and budgets (DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# Logs: the service instance's containers (docker's awslogs driver, a stream per
# service) and the machines', 30 days each. Metrics: from the NovaWorld
# server's heartbeat, a JSON line it logs every 60 s,
#   {"heartbeat":{"nw_sessions":N,"listed_hosts":N,"hosted_live":N,
#     "hosted_failed":N,"machines_lost":N,"launch_failures":N,
#     "billing_sync_age_s":N,"webhook_rejects":N,"limits_held":0|1}}
# (gauges, but launch_failures and webhook_rejects count the minute since the
# last line; billing_sync_age_s is -1 with billing off; limits_held is 1 while
# the supervisor holds back the entitlement stops, its mass-stop guard), and
# from its boot line. Alarms, to one SNS topic emailing var.alert_email:
#   heartbeat missing for 5 minutes (no data is an alarm: a dead server logs
#   nothing), more than 3 boots in 15 minutes (a crash loop), a failed hosted
#   server or a lost machine, 3 failed launches in 15 minutes, a billing sync
#   older than an hour, 5 refused webhooks in 15 minutes, the entitlement
#   stops held for 5 minutes, the service instance's status checks (system:
#   recover it; instance: reboot it), its CPU over 80% for 15 minutes, the
#   database's CPU credits and free storage, and (in us-east-1, where Route
#   53's metrics live) the /healthz health check.
# Budgets (prod, account-wide): the month's cost and its EC2 data transfer out,
# and Cost Anomaly Detection.

locals {
  metric_namespace = "OpenNova/${local.deployment}"
  alert_email      = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.alert_email)) ? var.alert_email : ""

  # The heartbeat's fields, by metric name.
  heartbeat_fields = {
    NwSessions      = "nw_sessions"
    ListedHosts     = "listed_hosts"
    HostedLive      = "hosted_live"
    HostedFailed    = "hosted_failed"
    MachinesLost    = "machines_lost"
    LaunchFailures  = "launch_failures"
    BillingSyncAgeS = "billing_sync_age_s"
    WebhookRejects  = "webhook_rejects"
    LimitsHeld      = "limits_held"
  }

  alarm_actions = [aws_sns_topic.alerts.arn]
}

# ---- logs ---------------------------------------------------------------------

# The service instance's containers (deploy/compose/docker-compose.prod.yml).
# `./deploy/run.sh app logs` reads it.
resource "aws_cloudwatch_log_group" "service" {
  name              = "/opennova/${local.deployment}/service"
  retention_in_days = var.log_retention_days

  tags = var.tags
}

# The game machines' (hostd and the slots), through the machine role.
resource "aws_cloudwatch_log_group" "machines" {
  name              = "/opennova/${local.deployment}/machines"
  retention_in_days = var.log_retention_days

  tags = var.tags
}

# ---- metrics from the server's lines ---------------------------------------

resource "aws_cloudwatch_log_metric_filter" "heartbeat" {
  name           = "${local.name}-heartbeat"
  log_group_name = aws_cloudwatch_log_group.service.name
  pattern        = "{ $.heartbeat.nw_sessions >= 0 }"

  metric_transformation {
    name      = "Heartbeat"
    namespace = local.metric_namespace
    value     = "1"
    unit      = "Count"
  }
}

resource "aws_cloudwatch_log_metric_filter" "heartbeat_field" {
  for_each       = local.heartbeat_fields
  name           = "${local.name}-heartbeat-${each.value}"
  log_group_name = aws_cloudwatch_log_group.service.name
  pattern        = "{ $.heartbeat.${each.value} >= 0 }"

  metric_transformation {
    name      = each.key
    namespace = local.metric_namespace
    value     = "$.heartbeat.${each.value}"
  }
}

# Each boot of the server logs its migrations line once.
resource "aws_cloudwatch_log_metric_filter" "boots" {
  name           = "${local.name}-boots"
  log_group_name = aws_cloudwatch_log_group.service.name
  pattern        = "\"[boot] migrations:\""

  metric_transformation {
    name      = "Boots"
    namespace = local.metric_namespace
    value     = "1"
    unit      = "Count"
  }
}

# ---- where the alarms go -------------------------------------------------------

resource "aws_sns_topic" "alerts" {
  name = "${local.name}-alerts"

  tags = var.tags
}

# An email subscription the address confirms once (AWS sends the link).
resource "aws_sns_topic_subscription" "alerts_email" {
  count     = local.alert_email != "" ? 1 : 0
  topic_arn = aws_sns_topic.alerts.arn
  protocol  = "email"
  endpoint  = local.alert_email
}

# ---- alarms on the server's metrics --------------------------------------------

locals {
  # name => metric, statistic, period, periods, comparison, threshold, missing
  # data, what it means.
  server_alarms = {
    heartbeat-missing = {
      metric      = "Heartbeat", statistic = "Sum", period = 300, periods = 1,
      comparison  = "LessThanThreshold", threshold = 1, missing = "breaching",
      description = "The NovaWorld server logged no heartbeat for 5 minutes: it is down, stalled or not logging (app status, app logs novaworld)."
    }
    crash-loop = {
      metric      = "Boots", statistic = "Sum", period = 900, periods = 1,
      comparison  = "GreaterThanThreshold", threshold = 3, missing = "notBreaching",
      description = "The NovaWorld server booted more than 3 times in 15 minutes (app logs novaworld)."
    }
    hosted-failed = {
      metric      = "HostedFailed", statistic = "Maximum", period = 300, periods = 1,
      comparison  = "GreaterThanThreshold", threshold = 0, missing = "notBreaching",
      description = "A hosted server is failed (the admin fleet view)."
    }
    machines-lost = {
      metric      = "MachinesLost", statistic = "Maximum", period = 300, periods = 1,
      comparison  = "GreaterThanThreshold", threshold = 0, missing = "notBreaching",
      description = "A game machine stopped reporting (fleet status)."
    }
    launch-failures = {
      metric      = "LaunchFailures", statistic = "Sum", period = 900, periods = 1,
      comparison  = "GreaterThanOrEqualToThreshold", threshold = 3, missing = "notBreaching",
      description = "3 or more machine launches failed in 15 minutes (the vCPU quota, capacity, the IAM policy: app logs novaworld)."
    }
    billing-sync-stale = {
      metric      = "BillingSyncAgeS", statistic = "Maximum", period = 300, periods = 2,
      comparison  = "GreaterThanThreshold", threshold = 3600, missing = "notBreaching",
      description = "The Stripe sync has not succeeded for over an hour (the admin billing view)."
    }
    webhook-rejects = {
      metric      = "WebhookRejects", statistic = "Sum", period = 900, periods = 1,
      comparison  = "GreaterThanOrEqualToThreshold", threshold = 5, missing = "notBreaching",
      description = "5 or more Stripe webhooks refused in 15 minutes (a rolled secret not synced, or forged requests)."
    }
    # Every heartbeat of 5 minutes said 1 (Minimum): a hold that lasts, not one
    # pass's. A server without the field logs nothing for it: no data is no
    # alarm (heartbeat-missing covers silence).
    limits-held = {
      metric      = "LimitsHeld", statistic = "Minimum", period = 300, periods = 1,
      comparison  = "GreaterThanOrEqualToThreshold", threshold = 1, missing = "notBreaching",
      description = "The entitlement stops have been held for 5 minutes (the supervisor's mass-stop guard): over-entitled servers keep running meanwhile (the admin billing view says why)."
    }
  }
}

resource "aws_cloudwatch_metric_alarm" "server" {
  for_each = local.server_alarms

  alarm_name          = "${local.name}-${each.key}"
  alarm_description   = each.value.description
  namespace           = local.metric_namespace
  metric_name         = each.value.metric
  statistic           = each.value.statistic
  period              = each.value.period
  evaluation_periods  = each.value.periods
  comparison_operator = each.value.comparison
  threshold           = each.value.threshold
  treat_missing_data  = each.value.missing
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# ---- alarms on the host and the database ------------------------------------------

# A failed system status check: EC2 recovers the instance onto other hardware
# (same instance, same network interface and address).
resource "aws_cloudwatch_metric_alarm" "host_recover" {
  alarm_name          = "${local.name}-host-system-check"
  alarm_description   = "The service instance failed its system status check: EC2 recovers it."
  namespace           = "AWS/EC2"
  metric_name         = "StatusCheckFailed_System"
  dimensions          = { InstanceId = aws_instance.service.id }
  statistic           = "Maximum"
  period              = 60
  evaluation_periods  = 2
  comparison_operator = "GreaterThanOrEqualToThreshold"
  threshold           = 1
  alarm_actions       = concat(["arn:${local.partition}:automate:${var.region}:ec2:recover"], local.alarm_actions)
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# A failed instance status check (the OS): reboot it.
resource "aws_cloudwatch_metric_alarm" "host_reboot" {
  alarm_name          = "${local.name}-host-instance-check"
  alarm_description   = "The service instance failed its instance status check: EC2 reboots it."
  namespace           = "AWS/EC2"
  metric_name         = "StatusCheckFailed_Instance"
  dimensions          = { InstanceId = aws_instance.service.id }
  statistic           = "Maximum"
  period              = 60
  evaluation_periods  = 3
  comparison_operator = "GreaterThanOrEqualToThreshold"
  threshold           = 1
  alarm_actions       = concat(["arn:${local.partition}:automate:${var.region}:ec2:reboot"], local.alarm_actions)
  ok_actions          = local.alarm_actions

  tags = var.tags
}

resource "aws_cloudwatch_metric_alarm" "host_cpu" {
  alarm_name          = "${local.name}-host-cpu"
  alarm_description   = "The service instance's CPU stayed above 80% for 15 minutes."
  namespace           = "AWS/EC2"
  metric_name         = "CPUUtilization"
  dimensions          = { InstanceId = aws_instance.service.id }
  statistic           = "Average"
  period              = 300
  evaluation_periods  = 3
  comparison_operator = "GreaterThanThreshold"
  threshold           = 80
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# The burstable database's CPU credits (a db.t4g.micro earns 12 an hour and
# holds at most 144): running low means it is about to be throttled.
resource "aws_cloudwatch_metric_alarm" "db_cpu_credits" {
  alarm_name          = "${local.name}-db-cpu-credits"
  alarm_description   = "The database's CPU credit balance is low: it is about to be throttled (a larger db_instance_class)."
  namespace           = "AWS/RDS"
  metric_name         = "CPUCreditBalance"
  dimensions          = { DBInstanceIdentifier = aws_db_instance.main.identifier }
  statistic           = "Minimum"
  period              = 300
  evaluation_periods  = 3
  comparison_operator = "LessThanThreshold"
  threshold           = 20
  treat_missing_data  = "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

resource "aws_cloudwatch_metric_alarm" "db_free_storage" {
  alarm_name          = "${local.name}-db-free-storage"
  alarm_description   = "The database has less than 2 GiB free (storage grows on its own up to db_max_allocated_storage_gb)."
  namespace           = "AWS/RDS"
  metric_name         = "FreeStorageSpace"
  dimensions          = { DBInstanceIdentifier = aws_db_instance.main.identifier }
  statistic           = "Minimum"
  period              = 300
  evaluation_periods  = 2
  comparison_operator = "LessThanThreshold"
  threshold           = 2147483648
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# ---- the outside view --------------------------------------------------------------

# Route 53's checkers, from around the world, fetch /healthz on the retail HTTP
# port at the Elastic IP: the path a stock client takes.
resource "aws_route53_health_check" "service" {
  ip_address        = local.service_public_ip
  port              = var.game_http_port
  type              = "HTTP"
  resource_path     = "/healthz"
  failure_threshold = 3
  request_interval  = 30

  tags = merge(var.tags, {
    Name = "${local.name}-healthz"
  })
}

# Route 53's health check metrics exist in us-east-1 alone.
resource "aws_cloudwatch_metric_alarm" "healthz" {
  count = var.region == "us-east-1" ? 1 : 0

  alarm_name          = "${local.name}-healthz"
  alarm_description   = "Route 53 cannot reach http://<Elastic IP>:${var.game_http_port}/healthz."
  namespace           = "AWS/Route53"
  metric_name         = "HealthCheckStatus"
  dimensions          = { HealthCheckId = aws_route53_health_check.service.id }
  statistic           = "Minimum"
  period              = 60
  evaluation_periods  = 3
  comparison_operator = "LessThanThreshold"
  threshold           = 1
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# ---- budgets (prod's, counting every OpenNova deployment) ------------------------------

# The account may hold other projects, so the budgets count only what carries an
# OpenNova deployment's tag (versions.tf's default tag, and the machines' launch
# tag): prod's and every deployment edge_other_deployments names. Cost
# Explorer sees the tag once it is an active cost allocation tag (DEPLOY.md,
# "Budgets"); until then these budgets read zero.
locals {
  opennova_deployment_tags = concat([local.hosted_deployment], [for o in var.edge_other_deployments : "${var.instance_name}-${o}"])
  budget_tag_filter        = [for d in local.opennova_deployment_tags : format("user:opennova:deployment$%s", d)]
}

resource "aws_budgets_budget" "monthly_cost" {
  count        = local.manage_shared ? 1 : 0
  name         = "opennova-monthly-cost"
  budget_type  = "COST"
  limit_amount = tostring(var.budget_monthly_usd)
  limit_unit   = "USD"
  time_unit    = "MONTHLY"

  cost_filter {
    name   = "TagKeyValue"
    values = local.budget_tag_filter
  }

  dynamic "notification" {
    for_each = local.alert_email != "" ? { ACTUAL = 80, FORECASTED = 100 } : {}
    content {
      notification_type          = notification.key
      comparison_operator        = "GREATER_THAN"
      threshold                  = notification.value
      threshold_type             = "PERCENTAGE"
      subscriber_email_addresses = [local.alert_email]
    }
  }
}

# The hosted servers' egress is metered, never capped: this is the alarm.
resource "aws_budgets_budget" "egress" {
  count        = local.manage_shared ? 1 : 0
  name         = "opennova-ec2-egress"
  budget_type  = "USAGE"
  limit_amount = tostring(var.budget_egress_gb)
  limit_unit   = "GB"
  time_unit    = "MONTHLY"

  cost_filter {
    name   = "UsageTypeGroup"
    values = ["EC2: Data Transfer - Internet (Out)"]
  }

  cost_filter {
    name   = "TagKeyValue"
    values = local.budget_tag_filter
  }

  dynamic "notification" {
    for_each = local.alert_email != "" ? { ACTUAL = 80, FORECASTED = 100 } : {}
    content {
      notification_type          = notification.key
      comparison_operator        = "GREATER_THAN"
      threshold                  = notification.value
      threshold_type             = "PERCENTAGE"
      subscriber_email_addresses = [local.alert_email]
    }
  }
}

resource "aws_ce_anomaly_monitor" "services" {
  count        = local.manage_shared && var.cost_anomaly_monitor ? 1 : 0
  name         = "opennova-services"
  monitor_type = "CUSTOM"
  # OpenNova's deployments' tag alone: the account holds other projects, whose
  # anomalies are not OpenNova's to email (and an account-wide AWS-services
  # monitor is one per account).
  monitor_specification = jsonencode({
    Tags = {
      Key          = "opennova:deployment"
      Values       = local.opennova_deployment_tags
      MatchOptions = ["EQUALS"]
    }
  })
}

resource "aws_ce_anomaly_subscription" "email" {
  count            = local.manage_shared && var.cost_anomaly_monitor && local.alert_email != "" ? 1 : 0
  name             = "opennova-cost-anomalies"
  frequency        = "DAILY"
  monitor_arn_list = [aws_ce_anomaly_monitor.services[0].arn]

  subscriber {
    type    = "EMAIL"
    address = local.alert_email
  }

  threshold_expression {
    dimension {
      key           = "ANOMALY_TOTAL_IMPACT_ABSOLUTE"
      match_options = ["GREATER_THAN_OR_EQUAL"]
      values        = [tostring(var.cost_anomaly_threshold_usd)]
    }
  }
}
