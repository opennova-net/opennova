# ---------------------------------------------------------------------------
# The service's six programs: gate, lobby, web, fleet, worker, data (service.tf
# says what they share; DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# Each program is its own image (<image_registry>/opennova-novaworld-<program>)
# and an ECS service on Fargate (x86_64, platform 1.4) in the public subnets of
# both zones, each task on a public IPv4 of its own (no NAT: the tasks reach
# the AWS APIs, GHCR and Stripe through it), in its program's security group,
# with its program's task role:
#
#   gate        UDP gate_udp_port, behind the public network load balancer
#               (load_balancer.tf), which checks its /livez on health_port
#   lobby       UDP nw_udp_port, behind the same balancer, which checks its
#               /livez on health_port: every lobby announces that one address
#               (ONNET_LOBBY_ENDPOINT, the gate's address and the lobby port),
#               which the gate hands each client
#   web         the retail HTTP routes and the website's API (game_http_port),
#               the update service of retail UPDATE.EXE (update_ports) and
#               the portal's nginx (443) in one task, behind the balancer;
#               nginx reaches the server on the task's loopback
#   fleet       the internal listener (internal_http_port, the leader alone) on
#               its task's private address, from the machines alone (fleet.tf),
#               named fleet.<deployment>.opennova.internal (cluster.tf); no
#               cloud call (its role holds ECS Exec alone)
#   worker      nothing inbound; the fleet's machine launches (fleet.tf), the
#               mail and billing, and every periodic job but the data sets'
#   data        nothing inbound; the data sets' jobs over the game data's
#               store (data_store), on 40 GiB of task storage
#
# The machines' agent, hostd, is no program here: a DAEMON service of the
# fleet cluster on the machines themselves (fleet_agent.tf), rolled with each
# release before the fleet program.
#
# Every program reaches the database (database.tf); web alone reaches the
# cache (cache.tf), and reads what the admin's infrastructure view shows (the
# services, tasks, target groups, database, cache, release and machines below,
# "web's infrastructure reads", and the fleet cluster's, fleet_agent.tf). Each container answers /livez (its stall watchdog) on
# health_port (web on game_http_port too, beside /healthz), which its ECS
# health check asks; a task failing it is replaced. A deployment rolls with
# the circuit breaker on (a deployment that never turns healthy rolls back)
# and starts every program's new tasks before it stops the old (the
# fleet's new task waits as the standby, then leads; the balancer sends
# new lobby flows to the new lobby task once it passes, and drains the old one
# before ECS stops it).
#
# Terraform makes each program's task definition, the template every deploy
# clones: `app deploy` registers a revision of it with the release's images
# (pinned by digest) and the secrets SSM holds, and points the service at it.
# So the services ignore their task definition here, and their task count,
# which Application Auto Scaling holds between each program's min and max
# (web and worker on their CPU); a changed template reaches the tasks at the
# next `app deploy` or `app converge`. A template names the secrets every
# deployment holds alone; the optional ones (the first admin, edge
# authentication's previous secret, billing's set) are in its
# `opennova.optional-secrets` label, which a deploy turns into secrets for
# those SSM holds. Until a release names a program (the release manifest's
# program list), it runs nothing: its service is made with no task and its
# count's floor is 0, so no task ever starts from a template's unpinned image,
# and its alarms stay off (observability.tf); the first `app deploy` naming it
# starts it at its desired count, and the next `infra apply` raises the floor
# to its min.

locals {
  programs = {
    gate   = var.gate
    lobby  = var.lobby
    web    = var.web
    fleet  = var.fleet
    worker = var.worker
    data   = var.data
  }

  # The images, under var.image_registry: the six programs', the portal's
  # nginx (web's second container) and the backup task's (backup.tf).
  program_images = { for p in keys(local.programs) : p => "opennova-novaworld-${p}" }
  portal_image   = "novaworld-web"
  backup_image   = "opennova-novaworld-backup"

  # The programs Application Auto Scaling scales on their CPU, and the
  # average it holds them at (the data program's jobs have global caps, so
  # more tasks would run none more at once: it never scales).
  cpu_scaled_programs = toset(["web", "worker"])
  cpu_target_percent  = 60

  # How a deployment replaces tasks (percent of the service's count): every
  # program starts the new first. A bad image costs nothing that runs: the
  # old tasks serve until the circuit breaker rolls the deployment back.
  deployment_percent = { for p in keys(local.programs) : p => { min = 100, max = 200 } }

  # The programs the deployment's release names: `app deploy` writes its
  # manifest (JSON naming the programs) over the parameter's "none" (or the
  # single service instance's release id, at the cutover). A program none names
  # yet (before the first release, or one a newer service added) runs no task
  # and alarms on nothing; `has_release` is whether any release is recorded.
  release_programs = try(tolist(jsondecode(local.release_value).programs), [])
  released         = { for p in keys(local.programs) : p => contains(local.release_programs, p) }
  has_release      = length(local.release_programs) > 0

  # Every program's tasks at its max and the extra tasks a deployment runs
  # beside them: the processes the database's max_connections must hold
  # (database.tf).
  program_processes_max = sum([for k, p in local.programs : ceil(p.max * local.deployment_percent[k].max / 100)])

  # Seconds a stopping container gets between SIGTERM and SIGKILL (Fargate
  # allows 120): each program's orderly shutdown (a lobby's drain and its
  # sessions' end among them: ECS signals it once the balancer drained it).
  stop_timeout = { gate = 30, lobby = 30, web = 30, fleet = 30, worker = 30, data = 30 }

  # Seconds a new container's failed health checks do not count: its boot
  # (the migrations under their lock among it) before its health listener
  # binds.
  health_start_period = { gate = 60, lobby = 60, web = 60, fleet = 60, worker = 60, data = 60 }

  program_subnet_ids = [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].id]

  # Fargate's task sizes: the memory (MiB) each CPU size (units) takes.
  fargate_memory = {
    "256"  = [512, 1024, 2048]
    "512"  = [1024, 2048, 3072, 4096]
    "1024" = range(2048, 8193, 1024)
    "2048" = range(4096, 16385, 1024)
    "4096" = range(8192, 30721, 1024)
  }
}

# ---- security groups -------------------------------------------------------------

# Their descriptions are fixed for each group's life (a changed one replaces
# the group). EC2 descriptions take letters, digits and . _-:/()#,@[]+=&;{}!$*
# alone: no apostrophe.
locals {
  program_descriptions = {
    gate   = "OpenNova NovaWorld gate: its UDP port and health port from the load balancer alone"
    lobby  = "OpenNova NovaWorld lobby: its UDP port to everyone"
    web    = "OpenNova NovaWorld web: 443 and the retail HTTP port from the load balancer alone"
    fleet  = "OpenNova NovaWorld fleet: the internal listener from the machines alone"
    worker = "OpenNova NovaWorld worker: nothing inbound"
    data   = "OpenNova NovaWorld data: nothing inbound"
  }
}

resource "aws_security_group" "program" {
  for_each    = local.programs
  name        = "${local.name}-${each.key}"
  description = local.program_descriptions[each.key]
  vpc_id      = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-${each.key}"
  })
}

# Out: the image pulls (ghcr.io), the AWS APIs, Stripe, the database and the
# cache.
resource "aws_vpc_security_group_egress_rule" "program_all" {
  for_each = local.programs

  security_group_id = aws_security_group.program[each.key].id
  description       = "All outbound"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}

# The load balancer's traffic and health checks, from its security group
# (load_balancer.tf), which stands for every client the balancer passes
# through with its own address.
resource "aws_vpc_security_group_ingress_rule" "from_lb" {
  for_each = local.lb_target_rules

  security_group_id            = aws_security_group.program[each.value.program].id
  description                  = each.value.description
  ip_protocol                  = each.value.protocol
  from_port                    = each.value.port
  to_port                      = each.value.port
  referenced_security_group_id = aws_security_group.lb.id
}

# The lobby's UDP port, fixed by the stock clients (the gate announces it),
# from everyone on the task's own address as well as through the balancer
# (whose group the lobby admits, load_balancer.tf). No SSH.
resource "aws_vpc_security_group_ingress_rule" "lobby" {
  for_each = toset(var.allowed_cidrs)

  security_group_id = aws_security_group.program["lobby"].id
  description       = "NovaWorld UDP, the lobby (the port the gate announces)"
  ip_protocol       = "udp"
  from_port         = var.nw_udp_port
  to_port           = var.nw_udp_port
  cidr_ipv4         = each.value
}

# ---- IAM: a task role per program --------------------------------------------------

resource "aws_iam_role" "program" {
  for_each             = local.programs
  name                 = "${local.name}-${each.key}"
  assume_role_policy   = data.aws_iam_policy_document.ecs_tasks_assume_role.json
  permissions_boundary = local.role_boundary_arn

  tags = merge(var.tags, {
    Name = "${local.name}-${each.key}"
  })
}

# ECS Exec (`app shell`, the toolbox's admin calls on web): the session's
# channels, which take no resource scope. Every program's and the backup
# task's role (backup.tf); the fleet program's holds nothing else. fleet.tf
# gives the worker the machines' EC2 calls, mail.tf web and the worker the
# mail, data_store below web and the data program the game data's store.
data "aws_iam_policy_document" "exec" {
  statement {
    sid    = "EcsExecChannels"
    effect = "Allow"
    actions = [
      "ssmmessages:CreateControlChannel",
      "ssmmessages:CreateDataChannel",
      "ssmmessages:OpenControlChannel",
      "ssmmessages:OpenDataChannel",
    ]
    resources = ["*"]
  }
}

resource "aws_iam_role_policy" "exec" {
  for_each = local.programs
  name     = "${local.name}-${each.key}-exec"
  role     = aws_iam_role.program[each.key].id
  policy   = data.aws_iam_policy_document.exec.json
}

# Web's infrastructure reads, for the admin's view (apps/novaworld_web/infra/):
# read-only, each scoped to this deployment's own resource where the API allows
# one. ListTasks takes the cluster as a condition; Elastic Load Balancing's and
# EC2's describe calls take no resource (web asks only for this deployment's
# target groups and its machines' instance ids). The boundary
# (infra/aws/iam/opennova-role-boundary.json) holds the same, so the org stack
# applies it first.
data "aws_iam_policy_document" "infra_read" {
  statement {
    sid       = "ReadServices"
    effect    = "Allow"
    actions   = ["ecs:DescribeServices"]
    resources = ["arn:${local.partition}:ecs:${local.region}:${local.account_id}:service/${aws_ecs_cluster.main.name}/*"]
  }

  statement {
    sid       = "ListTasks"
    effect    = "Allow"
    actions   = ["ecs:ListTasks"]
    resources = ["*"]

    condition {
      test     = "ArnEquals"
      variable = "ecs:cluster"
      values   = [aws_ecs_cluster.main.arn]
    }
  }

  statement {
    sid       = "DescribeTasks"
    effect    = "Allow"
    actions   = ["ecs:DescribeTasks"]
    resources = ["arn:${local.partition}:ecs:${local.region}:${local.account_id}:task/${aws_ecs_cluster.main.name}/*"]
  }

  statement {
    sid       = "ReadTargetHealth"
    effect    = "Allow"
    actions   = ["elasticloadbalancing:DescribeTargetHealth"]
    resources = ["*"]
  }

  statement {
    sid       = "ReadDatabase"
    effect    = "Allow"
    actions   = ["rds:DescribeDBInstances"]
    resources = [aws_db_instance.main.arn]
  }

  statement {
    sid       = "ReadCache"
    effect    = "Allow"
    actions   = ["elasticache:DescribeReplicationGroups"]
    resources = [aws_elasticache_replication_group.cache.arn]
  }

  statement {
    sid       = "ReadRelease"
    effect    = "Allow"
    actions   = ["ssm:GetParameter"]
    resources = [aws_ssm_parameter.release.arn]
  }

  statement {
    sid       = "ReadMachines"
    effect    = "Allow"
    actions   = ["ec2:DescribeInstances"]
    resources = ["*"]
  }
}

resource "aws_iam_role_policy" "infra_read" {
  name   = "${local.name}-web-infra-read"
  role   = aws_iam_role.program["web"].id
  policy = data.aws_iam_policy_document.infra_read.json
}

# The game data's store (ADR 0064; fleet.tf names it: the deployment's
# operations bucket under local.data_prefix, data/), with hosting on: web
# starts the admin page's uploads (multipart; the browser sends each part
# through the site and web puts it), removes an abandoned tree's and reads the
# packages the update service serves; the data program finalizes a tree,
# validates and applies a release (server-side copies among it) and prunes.
# Both read, write and delete under data/ alone, list it, and list the uploads
# in flight (that listing takes no prefix condition; aborting one is scoped by
# its key). The machines read the trees alone (fleet.tf); the fleet program
# needs no call (it builds URLs alone), nor does the worker.
data "aws_iam_policy_document" "data_store" {
  count = local.hosted_enabled ? 1 : 0

  statement {
    sid       = "ListDataStore"
    effect    = "Allow"
    actions   = ["s3:ListBucket"]
    resources = [local.ops_bucket_arn]

    condition {
      test     = "StringLike"
      variable = "s3:prefix"
      values   = ["${local.data_prefix}/*"]
    }
  }

  statement {
    sid       = "ListDataStoreUploads"
    effect    = "Allow"
    actions   = ["s3:ListBucketMultipartUploads"]
    resources = [local.ops_bucket_arn]
  }

  statement {
    sid       = "DataStoreObjects"
    effect    = "Allow"
    actions   = ["s3:GetObject", "s3:PutObject", "s3:DeleteObject", "s3:AbortMultipartUpload"]
    resources = ["${local.ops_bucket_arn}/${local.data_prefix}/*"]
  }
}

resource "aws_iam_role_policy" "data_store" {
  for_each = local.hosted_enabled ? toset(["web", "data"]) : toset([])

  name   = "${local.name}-${each.key}-data-store"
  role   = aws_iam_role.program[each.key].id
  policy = data.aws_iam_policy_document.data_store[0].json
}

# ---- what each program's container gets --------------------------------------------

locals {
  # Every program: its region, its health port, the database.
  common_env = merge({
    AWS_REGION         = local.region
    AWS_DEFAULT_REGION = local.region
    ONNET_HEALTH_PORT  = tostring(var.health_port)
  }, local.database_env)

  # The gate's address, which is also web's (the load balancer's zone-a
  # Elastic IP: POSTIPADDRESS, and the startup URL, the NWU 0x82 web domain,
  # HOST_URL and GSB_SERVER), and the ports the programs listen on and announce.
  public_env = {
    ONNET_PUBLIC_HOST   = local.service_public_ip
    ONNET_WEB_HOST      = local.service_public_ip
    ONNET_GATE_UDP_PORT = tostring(var.gate_udp_port)
    ONNET_NW_UDP_PORT   = tostring(var.nw_udp_port)
    ONNET_HTTP_PORT     = tostring(var.game_http_port)
  }

  # The lobby address every lobby announces (each its own instance behind it):
  # the gate's address, where the balancer's lobby listener spreads the
  # clients across the lobby tasks, and the lobby port.
  lobby_env = { ONNET_LOBBY_ENDPOINT = "${local.service_public_ip}:${var.nw_udp_port}" }

  # The site's origin (billing's returns and the mail's links), with Cloudflare.
  site_env = local.dns_enabled ? { ONNET_SITE_URL = "https://${local.web_host}" } : {}

  # Web behind its own nginx: the website cookie Secure on an https site, the
  # proxy whose X-Real-IP names the client (nginx on the task's loopback, and
  # the toolbox's admin calls from there), /api/ for it alone (the retail port
  # is open to the internet), and the retail port's own proxy, the portal
  # nginx's retail server (web/nginx.conf), which reaches web from 127.0.0.2
  # and names each client in X-Real-IP, no trusted proxy.
  web_env = {
    ONNET_COOKIE_SECURE   = local.edge_proxied ? "1" : "0"
    ONNET_TRUSTED_PROXIES = "127.0.0.1"
    ONNET_API_PROXY_ONLY  = "1"
    ONNET_RETAIL_PROXY    = "127.0.0.2"
  }

  # The admin's infrastructure view (apps/novaworld_web/infra/infra_status.h):
  # what web reads with its task role (the policy below, and fleet_agent.tf's),
  # each setting naming this deployment's own: the cluster and its services,
  # the fleet cluster ("" while hosting is off), the balancer's target groups
  # by listener, the database, the cache and the release parameter.
  infra_status_env = {
    ONNET_INFRA_REGION            = local.region
    ONNET_INFRA_CLUSTER           = aws_ecs_cluster.main.name
    ONNET_INFRA_FLEET_CLUSTER     = local.hosted_enabled ? aws_ecs_cluster.fleet[0].name : ""
    ONNET_INFRA_TARGET_GROUPS     = jsonencode({ for k, g in aws_lb_target_group.public : k => g.arn })
    ONNET_INFRA_DATABASE          = aws_db_instance.main.identifier
    ONNET_INFRA_CACHE             = aws_elasticache_replication_group.cache.replication_group_id
    ONNET_INFRA_RELEASE_PARAMETER = aws_ssm_parameter.release.name
  }

  # The update service (ADR 0061, 0064): web serves retail UPDATE.EXE on the
  # updater's ports behind the balancer (load_balancer.tf), at the update
  # record's name (edge.tf); without a zone there is no name, and an empty
  # host leaves the listener off.
  update_env = {
    ONNET_UPDATE_HOST  = local.update_host
    ONNET_UPDATE_PORTS = join(",", [for p in var.update_ports : tostring(p)])
  }

  # Hosting (fleet.tf): the fleet's policy for the programs that decide
  # whether hosting is on (web, the fleet, the worker), the machine driver's
  # own settings for the worker alone, which launches the machines, and the
  # game data's store for the programs that read it. The release adds the
  # serve image to every container that reads the policy and the hostd image
  # to the worker's (`app deploy --serve-tag --hostd-tag`, pinned by digest).
  program_env = {
    gate   = merge(local.common_env, local.public_env)
    lobby  = merge(local.common_env, local.public_env, local.lobby_env)
    web    = merge(local.common_env, local.public_env, local.site_env, local.web_env, local.update_env, local.cache_env, local.mail_env, local.hosted_policy_env, local.infra_status_env)
    fleet  = merge(local.common_env, local.public_env, local.hosted_policy_env)
    worker = merge(local.common_env, local.public_env, local.site_env, local.mail_env, local.hosted_policy_env, local.hosted_driver_env)
    data   = merge(local.common_env, local.store_env)
  }

  # The secrets each reads, SecureStrings `secrets sync` writes under
  # /opennova/<deployment>/secret/: the ones every deployment holds (`secrets
  # init` makes them), which the template names, and the optional ones (the
  # "# optional" and "# billing" lines of deploy/env/runtime.env.tpl), which
  # its label names for a deploy to add while SSM holds them.
  billing_secrets = ["STRIPE_SECRET_KEY", "STRIPE_WEBHOOK_SECRET", "STRIPE_PRICE_TIER1", "STRIPE_PRICE_TIER2", "STRIPE_PRICE_TIER3", "STRIPE_PRICE_OWN_MACHINE"]
  program_secrets = {
    gate   = ["DATABASE_PASSWORD"]
    lobby  = ["DATABASE_PASSWORD"]
    web    = ["DATABASE_PASSWORD", "ADMIN_API_TOKEN", "ONNET_REDIS_PASSWORD"]
    fleet  = ["DATABASE_PASSWORD"]
    worker = ["DATABASE_PASSWORD"]
    data   = ["DATABASE_PASSWORD"]
  }
  program_optional_secrets = {
    gate   = []
    lobby  = []
    web    = concat(["ONNET_BOOTSTRAP_ADMIN"], local.billing_secrets)
    fleet  = []
    worker = local.billing_secrets
    data   = []
  }

  # The label a deploy reads the optional secrets from: name to parameter ARN.
  optional_secrets_label = "opennova.optional-secrets"

  # Where each answers /livez: web on its HTTP port, the others on the
  # health port.
  livez_url = { for p in keys(local.programs) : p => "http://127.0.0.1:${p == "web" ? var.game_http_port : var.health_port}/livez" }

  program_ports = {
    gate   = [{ port = var.gate_udp_port, protocol = "udp" }, { port = var.health_port, protocol = "tcp" }]
    lobby  = [{ port = var.nw_udp_port, protocol = "udp" }, { port = var.health_port, protocol = "tcp" }]
    web    = concat([{ port = var.game_http_port, protocol = "tcp" }, { port = var.health_port, protocol = "tcp" }], [for p in var.update_ports : { port = p, protocol = "tcp" }])
    fleet  = [{ port = var.internal_http_port, protocol = "tcp" }, { port = var.health_port, protocol = "tcp" }]
    worker = [{ port = var.health_port, protocol = "tcp" }]
    data   = [{ port = var.health_port, protocol = "tcp" }]
  }

  # The data program's task storage, in GiB: a tree's files and a release's
  # package pass through its disk (the finalize, validate and apply jobs, a GiB
  # or two each at once; Fargate's default is 20). The others keep the default.
  task_ephemeral_storage_gib = { data = 40 }

  # The service's log group, a stream per container and task under the
  # program's prefix (<program>/<container>/<task id>; `app logs PROGRAM`).
  awslogs = {
    "awslogs-group"   = aws_cloudwatch_log_group.service.name
    "awslogs-region"  = local.region
    "mode"            = "non-blocking"
    "max-buffer-size" = "4m"
  }

  program_containers = {
    for p in keys(local.programs) : p => {
      name         = p
      image        = "${var.image_registry}/${local.program_images[p]}:${var.image_tag}"
      essential    = true
      portMappings = [for m in local.program_ports[p] : { containerPort = m.port, hostPort = m.port, protocol = m.protocol }]
      environment  = [for n in sort(keys(local.program_env[p])) : { name = n, value = local.program_env[p][n] }]
      secrets      = [for n in sort(local.program_secrets[p]) : { name = n, valueFrom = "${local.ssm_parameter_prefix}/secret/${n}" }]
      dockerLabels = { (local.optional_secrets_label) = jsonencode({ for n in local.program_optional_secrets[p] : n => "${local.ssm_parameter_prefix}/secret/${n}" }) }
      # An init as PID 1 forwards the signals: the server, as PID 1, would
      # ignore a second SIGTERM (its handler restores the default action, which
      # the kernel drops for a namespace's init).
      linuxParameters = { initProcessEnabled = true }
      stopTimeout     = local.stop_timeout[p]
      healthCheck = {
        command     = ["CMD", "curl", "-fsS", "-o", "/dev/null", "--max-time", "2", local.livez_url[p]]
        interval    = 10
        timeout     = 3
        retries     = 3
        startPeriod = local.health_start_period[p]
      }
      logConfiguration = {
        logDriver = "awslogs"
        options   = merge(local.awslogs, { "awslogs-stream-prefix" = p })
      }
    }
  }

  # Web's second container: the portal's nginx on 443 with the Cloudflare
  # Origin CA certificate (edge.tf, SSM tls/*, which Terraform makes beside
  # the proxied records; its entrypoint writes it under ORIGIN_TLS_DIR; without
  # it, nginx listens on 80 and the balancer has no 443) and edge
  # authentication, /api/ proxied to the server on 127.0.0.1; and on
  # portal_retail_port the retail port's server (load_balancer.tf), each
  # request's body capped, proxied to the server from 127.0.0.2. It starts
  # once the server answers /livez.
  portal_secrets = concat(
    [{ name = "EDGE_AUTH_SECRET", valueFrom = "${local.ssm_parameter_prefix}/secret/EDGE_AUTH_SECRET" }],
    local.edge_proxied ? [for n in ["ORIGIN_TLS_CERT", "ORIGIN_TLS_KEY"] : { name = n, valueFrom = "${local.ssm_parameter_prefix}/tls/${n}" }] : [],
  )
  portal_container = {
    name            = "portal"
    image           = "${var.image_registry}/${local.portal_image}:${var.image_tag}"
    essential       = true
    portMappings    = [for p in [443, local.portal_retail_port] : { containerPort = p, hostPort = p, protocol = "tcp" }]
    environment     = [{ name = "ORIGIN_TLS_DIR", value = "/run/tls" }]
    secrets         = local.portal_secrets
    dockerLabels    = { (local.optional_secrets_label) = jsonencode({ EDGE_AUTH_SECRET_PREVIOUS = "${local.ssm_parameter_prefix}/secret/EDGE_AUTH_SECRET_PREVIOUS" }) }
    dependsOn       = [{ containerName = "web", condition = "HEALTHY" }]
    linuxParameters = { initProcessEnabled = true }
    stopTimeout     = 30
    logConfiguration = {
      logDriver = "awslogs"
      options   = merge(local.awslogs, { "awslogs-stream-prefix" = "web" })
    }
  }

  container_definitions = merge(
    { for p in keys(local.programs) : p => jsonencode([local.program_containers[p]]) if p != "web" },
    { web = jsonencode([local.program_containers["web"], local.portal_container]) },
  )
}

# ---- task definitions and services --------------------------------------------------

resource "aws_ecs_task_definition" "program" {
  for_each = local.programs

  family                   = "${local.name}-${each.key}"
  requires_compatibilities = ["FARGATE"]
  network_mode             = "awsvpc"
  cpu                      = tostring(each.value.cpu)
  memory                   = tostring(each.value.memory)
  execution_role_arn       = aws_iam_role.execution.arn
  task_role_arn            = aws_iam_role.program[each.key].arn
  container_definitions    = local.container_definitions[each.key]

  # The origin certificate's parameters exist before a task can name them.
  depends_on = [aws_ssm_parameter.origin_tls_cert, aws_ssm_parameter.origin_tls_key]

  runtime_platform {
    operating_system_family = "LINUX"
    cpu_architecture        = "X86_64"
  }

  dynamic "ephemeral_storage" {
    for_each = contains(keys(local.task_ephemeral_storage_gib), each.key) ? [local.task_ephemeral_storage_gib[each.key]] : []
    content {
      size_in_gib = ephemeral_storage.value
    }
  }

  tags = merge(var.tags, {
    Name = "${local.name}-${each.key}"
  })

  lifecycle {
    precondition {
      condition     = contains(lookup(local.fargate_memory, tostring(each.value.cpu), []), each.value.memory)
      error_message = "${each.key}: cpu ${each.value.cpu} with memory ${each.value.memory} is no Fargate task size (256 takes 512, 1024 or 2048; 512 takes 1024 to 4096; 1024 takes 2048 to 8192, by 1024)."
    }
  }
}

resource "aws_ecs_service" "program" {
  for_each = local.programs

  name            = each.key
  cluster         = aws_ecs_cluster.main.id
  task_definition = aws_ecs_task_definition.program[each.key].arn
  # No task before a release names the program (the template's image is
  # unpinned); the first deploy naming it starts it at its desired count.
  desired_count          = local.released[each.key] ? each.value.desired : 0
  launch_type            = "FARGATE"
  platform_version       = "1.4.0"
  enable_execute_command = true
  # The tasks carry the service's tags (the deployment's, which the budgets
  # count) and ECS's own.
  enable_ecs_managed_tags = true
  propagate_tags          = "SERVICE"

  deployment_minimum_healthy_percent = local.deployment_percent[each.key].min
  deployment_maximum_percent         = local.deployment_percent[each.key].max

  deployment_circuit_breaker {
    enable   = true
    rollback = true
  }

  # A new task behind the balancer gets its boot (the migrations under their
  # lock among it) before the balancer's checks count.
  health_check_grace_period_seconds = length([for l in local.lb_listeners : l if l.program == each.key]) > 0 ? 120 : null

  network_configuration {
    subnets          = local.program_subnet_ids
    security_groups  = [aws_security_group.program[each.key].id]
    assign_public_ip = true
  }

  dynamic "load_balancer" {
    for_each = { for k, l in local.lb_listeners : k => l if l.program == each.key }
    content {
      target_group_arn = aws_lb_target_group.public[load_balancer.key].arn
      container_name   = load_balancer.value.container
      container_port   = load_balancer.value.target_port
    }
  }

  dynamic "service_registries" {
    for_each = each.key == "fleet" ? [aws_service_discovery_service.fleet.arn] : []
    content {
      registry_arn = service_registries.value
    }
  }

  # Terraform does not wait for the tasks: a service serves its release once
  # `app deploy` has run.
  wait_for_steady_state = false

  tags = merge(var.tags, {
    Name = "${local.name}-${each.key}"
  })

  lifecycle {
    # The deploy's revisions, and Application Auto Scaling's count.
    ignore_changes = [task_definition, desired_count]

    precondition {
      condition     = each.value.min >= 1 && each.value.min <= each.value.desired && each.value.desired <= each.value.max
      error_message = "${each.key}: its task counts must hold 1 <= min <= desired <= max (min ${each.value.min}, desired ${each.value.desired}, max ${each.value.max})."
    }
  }

  depends_on = [aws_lb_listener.public, aws_iam_role_policy.execution]
}

# ---- task counts ----------------------------------------------------------------------

# Every service's count held between its program's min and max (its floor 0
# until a release names the program, so nothing starts a task from a template).
resource "aws_appautoscaling_target" "program" {
  for_each = local.programs

  service_namespace  = "ecs"
  resource_id        = "service/${aws_ecs_cluster.main.name}/${aws_ecs_service.program[each.key].name}"
  scalable_dimension = "ecs:service:DesiredCount"
  min_capacity       = local.released[each.key] ? each.value.min : 0
  max_capacity       = each.value.max

  tags = var.tags
}

# Web and the worker follow their CPU: a task more past cpu_target_percent on
# average, a task fewer (after 5 quiet minutes) under it.
resource "aws_appautoscaling_policy" "cpu" {
  for_each = local.cpu_scaled_programs

  name               = "${local.name}-${each.key}-cpu"
  policy_type        = "TargetTrackingScaling"
  service_namespace  = aws_appautoscaling_target.program[each.key].service_namespace
  resource_id        = aws_appautoscaling_target.program[each.key].resource_id
  scalable_dimension = aws_appautoscaling_target.program[each.key].scalable_dimension

  target_tracking_scaling_policy_configuration {
    target_value       = local.cpu_target_percent
    scale_in_cooldown  = 300
    scale_out_cooldown = 60

    predefined_metric_specification {
      predefined_metric_type = "ECSServiceAverageCPUUtilization"
    }
  }
}
