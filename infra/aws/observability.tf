# ---------------------------------------------------------------------------
# Observability and budgets (DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# Logs: every task's containers (the awslogs driver, a stream per container
# and task under its program's prefix, <program>/<container>/<task id>, and
# backup/ for the backup task) and the machines', 30 days each. Metrics: from
# each program's heartbeat, a JSON line it logs every 60 s, carrying its program
# and the fields that program knows,
#   {"heartbeat":{"program":"gate|lobby|web|controller|worker","nw_sessions":N,
#     "listed_hosts":N,"hosted_live":N,"hosted_failed":N,"machines_lost":N,
#     "launch_failures":N,"billing_sync_age_s":N,"webhook_rejects":N,
#     "limits_held":0|1,"probes_unanswered":N,"lobbies_live":N}}
# (gauges, but launch_failures, webhook_rejects and probes_unanswered count
# the minute since the last line; billing_sync_age_s is -1 with billing off;
# limits_held is 1 while the supervisor holds back the entitlement stops, its
# mass-stop guard; probes_unanswered counts the gate's probes it had no lobby
# to hand out for, which no other light shows: that gate's loops turn and its
# /livez says ok), and
# from its boot line, {"boot":{"program":"..."}}. Alarms, to one SNS topic
# emailing var.alert_email:
#   per program: no heartbeat for 5 minutes (no data is an alarm: a dead
#   process logs nothing; before the first release, when no task runs, it is
#   none), more boots in 15 minutes than its max and 3 (a
#   crash loop), fewer tasks running than its min for 5 minutes, its CPU or its
#   memory over 80% for 15 minutes; a load balancer target group with an
#   unhealthy target for 10 minutes; a failed hosted server or a lost machine,
#   3 failed launches in 15 minutes, a billing sync older than an hour, 5
#   refused webhooks in 15 minutes, the entitlement stops held for 5 minutes,
#   a gate probe left unanswered for want of a lobby in 5 minutes;
#   the database's CPU, CPU credits (a burstable class), connections and free
#   storage, each cache node's evictions and CPU credits, and (in us-east-1,
#   where Route 53's metrics live) a health check of web's /healthz at the
#   gate's address, the way a stock client comes. A task failing its health
#   check is its service's to replace.
# Budgets (the deployment's account, which holds it alone): the month's cost
# and its EC2 data transfer out, and Cost Anomaly Detection.

locals {
  metric_namespace = "OpenNova/${local.deployment}"
  alert_email      = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.alert_email)) ? var.alert_email : ""

  # The heartbeat's fields, by metric name.
  heartbeat_fields = {
    NwSessions       = "nw_sessions"
    ListedHosts      = "listed_hosts" # every lobby logs the deployment-wide total: read it with Maximum, never Sum
    HostedLive       = "hosted_live"
    HostedFailed     = "hosted_failed"
    MachinesLost     = "machines_lost"
    LaunchFailures   = "launch_failures"
    BillingSyncAgeS  = "billing_sync_age_s"
    WebhookRejects   = "webhook_rejects"
    LimitsHeld       = "limits_held"
    ProbesUnanswered = "probes_unanswered"
    LobbiesLive      = "lobbies_live" # every gate logs the deployment-wide count: read it with Maximum, never Sum
  }

  alarm_actions = [aws_sns_topic.alerts.arn]
}

# ---- logs ---------------------------------------------------------------------

# Every task's containers (programs.tf, backup.tf), a stream each,
# <program>/<container>/<task id>. `./deploy/run.sh app logs` reads it.
resource "aws_cloudwatch_log_group" "service" {
  name              = "/opennova/${local.deployment}/service"
  retention_in_days = var.log_retention_days

  tags = var.tags
}

# The game machines' (hostd and the slots), through the machine role: a
# stream per slot, <machine id>/slot-<k>, and one for hostd, <machine id>/hostd
# (apps/hostd/README.md, "Logs"). `./deploy/run.sh fleet logs` reads it.
resource "aws_cloudwatch_log_group" "machines" {
  name              = "/opennova/${local.deployment}/machines"
  retention_in_days = var.log_retention_days

  tags = var.tags
}

# ---- metrics from the server's lines ---------------------------------------

# Each program's processes log the heartbeat with their program in it,
# {"heartbeat":{"program":"gate",...}}: one heartbeat a minute per process, by
# program (the Program dimension).
resource "aws_cloudwatch_log_metric_filter" "heartbeat" {
  for_each       = local.programs
  name           = "${local.name}-heartbeat-${each.key}"
  log_group_name = aws_cloudwatch_log_group.service.name
  pattern        = "{ $.heartbeat.program = \"${each.key}\" }"

  metric_transformation {
    name       = "Heartbeat"
    namespace  = local.metric_namespace
    value      = "1"
    unit       = "Count"
    dimensions = { Program = "$.heartbeat.program" }
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

# Each process logs one boot line as it starts, {"boot":{"program":"gate"}}:
# its program's boots.
resource "aws_cloudwatch_log_metric_filter" "boots" {
  for_each       = local.programs
  name           = "${local.name}-boots-${each.key}"
  log_group_name = aws_cloudwatch_log_group.service.name
  pattern        = "{ $.boot.program = \"${each.key}\" }"

  metric_transformation {
    name       = "Boots"
    namespace  = local.metric_namespace
    value      = "1"
    unit       = "Count"
    dimensions = { Program = "$.boot.program" }
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
      description = "3 or more machine launches failed in 15 minutes (the vCPU quota, capacity, the IAM policy: app logs worker)."
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
    # pass's. A process without the field logs nothing for it: no data is no
    # alarm (each program's heartbeat alarm covers silence).
    limits-held = {
      metric      = "LimitsHeld", statistic = "Minimum", period = 300, periods = 1,
      comparison  = "GreaterThanOrEqualToThreshold", threshold = 1, missing = "notBreaching",
      description = "The entitlement stops have been held for 5 minutes (the supervisor's mass-stop guard): over-entitled servers keep running meanwhile (the admin billing view says why)."
    }
    # A gate that has no lobby to hand out (none takes clients, no fallback)
    # answers no probe: no client can log in, while every health light is
    # green. Its heartbeat counts them; no data (no gate) is the heartbeat
    # alarm's.
    probes-unanswered = {
      metric      = "ProbesUnanswered", statistic = "Sum", period = 300, periods = 1,
      comparison  = "GreaterThanThreshold", threshold = 0, missing = "notBreaching",
      description = "A gate left probes unanswered in 5 minutes: it had no lobby to hand out (no lobby instance takes clients; LobbiesLive, app status, app logs lobby)."
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

# ---- alarms on each program -------------------------------------------------------

# No heartbeat from any of a program's processes for 5 minutes (no data is an
# alarm: a dead process logs nothing). Before the first release no task runs
# and none should log: no data is none (local.released, programs.tf), so a
# fresh deployment mails nothing.
resource "aws_cloudwatch_metric_alarm" "program_heartbeat" {
  for_each = local.programs

  alarm_name          = "${local.name}-${each.key}-heartbeat-missing"
  alarm_description   = "No ${each.key} process logged a heartbeat for 5 minutes: the program is down, stalled or not logging (app status, app logs ${each.key})."
  namespace           = local.metric_namespace
  metric_name         = "Heartbeat"
  dimensions          = { Program = each.key }
  statistic           = "Sum"
  period              = 300
  evaluation_periods  = 1
  comparison_operator = "LessThanThreshold"
  threshold           = 1
  treat_missing_data  = local.released ? "breaching" : "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# A deploy restarts each of a program's tasks once; more boots than its max
# and 3 besides, in 15 minutes, is a process starting over and over.
resource "aws_cloudwatch_metric_alarm" "program_crash_loop" {
  for_each = local.programs

  alarm_name          = "${local.name}-${each.key}-crash-loop"
  alarm_description   = "The ${each.key} program's processes booted more than ${each.value.max + 3} times in 15 minutes (app logs ${each.key})."
  namespace           = local.metric_namespace
  metric_name         = "Boots"
  dimensions          = { Program = each.key }
  statistic           = "Sum"
  period              = 900
  evaluation_periods  = 1
  comparison_operator = "GreaterThanThreshold"
  threshold           = each.value.max + 3
  treat_missing_data  = "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# Fewer tasks running than the program's min, every minute for 5 minutes:
# ECS's service CPU metric takes one sample a minute from each running task,
# so its SampleCount is the running count (no data, none running, is an
# alarm). Before the first release no task runs: the floor is 0 and no data is
# none.
resource "aws_cloudwatch_metric_alarm" "program_running" {
  for_each = local.programs

  alarm_name          = "${local.name}-${each.key}-running"
  alarm_description   = "Fewer than ${each.value.min} ${each.key} task(s) ran for 5 minutes: ECS cannot start them, or they fail as they start (app status, app logs ${each.key})."
  namespace           = "AWS/ECS"
  metric_name         = "CPUUtilization"
  dimensions          = { ClusterName = aws_ecs_cluster.main.name, ServiceName = aws_ecs_service.program[each.key].name }
  statistic           = "SampleCount"
  period              = 60
  evaluation_periods  = 5
  comparison_operator = "LessThanThreshold"
  threshold           = local.released ? each.value.min : 0
  treat_missing_data  = local.released ? "breaching" : "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# Each program's CPU and memory, its tasks' average against their size.
locals {
  program_utilization_alarms = merge(
    { for p in keys(local.programs) : "${p}-cpu" => { program = p, metric = "CPUUtilization", what = "CPU" } },
    { for p in keys(local.programs) : "${p}-memory" => { program = p, metric = "MemoryUtilization", what = "memory" } },
  )
}

resource "aws_cloudwatch_metric_alarm" "program_utilization" {
  for_each = local.program_utilization_alarms

  alarm_name          = "${local.name}-${each.key}"
  alarm_description   = "The ${each.value.program} tasks averaged above 80% of their ${each.value.what} for 15 minutes (a larger ${each.value.program}.cpu or .memory, or a higher max)."
  namespace           = "AWS/ECS"
  metric_name         = each.value.metric
  dimensions          = { ClusterName = aws_ecs_cluster.main.name, ServiceName = aws_ecs_service.program[each.value.program].name }
  statistic           = "Average"
  period              = 300
  evaluation_periods  = 3
  comparison_operator = "GreaterThanThreshold"
  threshold           = 80
  treat_missing_data  = "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# A target the load balancer has stopped sending to, for 10 minutes: a gate
# task (UDP), a lobby task (UDP), web's portal (443, the retail port) or web's
# server (the update service), one alarm per target group.
resource "aws_cloudwatch_metric_alarm" "lb_unhealthy" {
  for_each = local.lb_listeners

  alarm_name          = "${local.name}-${each.key}-unhealthy"
  alarm_description   = "A ${each.value.program} task failed the load balancer's ${each.key} health check for 10 minutes: it gets no traffic (app status, app logs ${each.value.program})."
  namespace           = "AWS/NetworkELB"
  metric_name         = "UnHealthyHostCount"
  dimensions          = { LoadBalancer = aws_lb.public.arn_suffix, TargetGroup = aws_lb_target_group.public[each.key].arn_suffix }
  statistic           = "Maximum"
  period              = 300
  evaluation_periods  = 2
  comparison_operator = "GreaterThanThreshold"
  threshold           = 0
  treat_missing_data  = "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# ---- alarms on the database and the cache -----------------------------------------

# The database's CPU.
resource "aws_cloudwatch_metric_alarm" "db_cpu" {
  alarm_name          = "${local.name}-db-cpu"
  alarm_description   = "The database's CPU stayed above 80% for 15 minutes (a larger db_instance_class, or Performance Insights' top statements)."
  namespace           = "AWS/RDS"
  metric_name         = "CPUUtilization"
  dimensions          = { DBInstanceIdentifier = aws_db_instance.main.identifier }
  statistic           = "Average"
  period              = 300
  evaluation_periods  = 3
  comparison_operator = "GreaterThanThreshold"
  threshold           = 80
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# A burstable database's CPU credits (a db.t4g.small earns 24 an hour and holds
# at most 576). RDS runs its T classes in unlimited mode: past a spent balance
# it is not throttled but bills surplus credits by the vCPU-hour, so a low
# balance means hours of load past the class's baseline.
resource "aws_cloudwatch_metric_alarm" "db_cpu_credits" {
  count = startswith(var.db_instance_class, "db.t") ? 1 : 0

  alarm_name          = "${local.name}-db-cpu-credits"
  alarm_description   = "The database's CPU credit balance is low: past it, its load is billed as surplus credits (a larger db_instance_class, or Performance Insights' top statements)."
  namespace           = "AWS/RDS"
  metric_name         = "CPUCreditBalance"
  dimensions          = { DBInstanceIdentifier = aws_db_instance.main.identifier }
  statistic           = "Minimum"
  period              = 300
  evaluation_periods  = 3
  comparison_operator = "LessThanThreshold"
  threshold           = 50
  treat_missing_data  = "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# Connections near max_connections (database.tf's sizing): a new process would
# be refused its pool.
resource "aws_cloudwatch_metric_alarm" "db_connections" {
  alarm_name          = "${local.name}-db-connections"
  alarm_description   = "The database's connections stayed above 80% of max_connections (${local.db_max_connections}) for 15 minutes: more program processes than db_service_processes plans for."
  namespace           = "AWS/RDS"
  metric_name         = "DatabaseConnections"
  dimensions          = { DBInstanceIdentifier = aws_db_instance.main.identifier }
  statistic           = "Maximum"
  period              = 300
  evaluation_periods  = 3
  comparison_operator = "GreaterThanThreshold"
  threshold           = floor(local.db_max_connections * 0.8)
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

# The cache's nodes (cache.tf): a node evicting keys (it is too small: a
# session or a pin lost before its TTL) and a burstable node's CPU credits.
resource "aws_cloudwatch_metric_alarm" "cache_evictions" {
  for_each = toset(local.cache_nodes)

  alarm_name          = "${each.key}-evictions"
  alarm_description   = "Cache node ${each.key} evicted keys: it is full, and sessions, pins or rate limits go before their TTL (a larger cache_node_type)."
  namespace           = "AWS/ElastiCache"
  metric_name         = "Evictions"
  dimensions          = { CacheClusterId = each.key }
  statistic           = "Sum"
  period              = 300
  evaluation_periods  = 1
  comparison_operator = "GreaterThanThreshold"
  threshold           = 0
  treat_missing_data  = "notBreaching"
  alarm_actions       = local.alarm_actions
  ok_actions          = local.alarm_actions

  tags = var.tags
}

resource "aws_cloudwatch_metric_alarm" "cache_cpu_credits" {
  for_each = startswith(var.cache_node_type, "cache.t") ? toset(local.cache_nodes) : toset([])

  alarm_name          = "${each.key}-cpu-credits"
  alarm_description   = "Cache node ${each.key}'s CPU credit balance is low: it is about to be throttled (a larger cache_node_type)."
  namespace           = "AWS/ElastiCache"
  metric_name         = "CPUCreditBalance"
  dimensions          = { CacheClusterId = each.key }
  statistic           = "Minimum"
  period              = 300
  evaluation_periods  = 3
  comparison_operator = "LessThanThreshold"
  threshold           = 10
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
# port at the gate's address (the load balancer's zone-a address): the path a
# stock client takes.
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
  count = local.region == "us-east-1" ? 1 : 0

  alarm_name          = "${local.name}-healthz"
  alarm_description   = "Route 53 cannot reach http://${local.service_public_ip}:${var.game_http_port}/healthz: no web task answers."
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

# ---- budgets (the deployment's account) ----------------------------------------------

# The account holds this deployment alone, so its budgets and its anomaly
# monitor count all of it: no tag filter, no cost allocation tag to activate.
resource "aws_budgets_budget" "monthly_cost" {
  name         = "${local.name}-monthly-cost"
  budget_type  = "COST"
  limit_amount = tostring(var.budget_monthly_usd)
  limit_unit   = "USD"
  time_unit    = "MONTHLY"

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
  name         = "${local.name}-ec2-egress"
  budget_type  = "USAGE"
  limit_amount = tostring(var.budget_egress_gb)
  limit_unit   = "GB"
  time_unit    = "MONTHLY"

  cost_filter {
    name   = "UsageTypeGroup"
    values = ["EC2: Data Transfer - Internet (Out)"]
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

# The account's one AWS-services monitor: every service's spend, each judged
# against its own history.
resource "aws_ce_anomaly_monitor" "services" {
  count             = var.cost_anomaly_monitor ? 1 : 0
  name              = "${local.name}-services"
  monitor_type      = "DIMENSIONAL"
  monitor_dimension = "SERVICE"
}

resource "aws_ce_anomaly_subscription" "email" {
  count            = var.cost_anomaly_monitor && local.alert_email != "" ? 1 : 0
  name             = "${local.name}-cost-anomalies"
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
