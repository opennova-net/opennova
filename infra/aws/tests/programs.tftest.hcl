# The service's six programs (programs.tf, cluster.tf): their task
# definitions, services, task counts, security groups, task roles, the
# execution role and the fleet program's Cloud Map name, planned against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare. From
# the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Six Fargate task definitions (x86_64, awsvpc), each its program's image,
# small (0.25 vCPU and 0.5 GiB; web and the worker 0.5 and 1; data 0.25 and 1,
# its task with 40 GiB of storage, the others Fargate's 20), health-checked
# on /livez, an init as PID 1, its logs under its program's prefix, and the
# environment and secrets its program reads and no other's (web's with the
# portal's nginx beside it, which starts once the server answers, and the
# update service's host and ports); six
# services on platform 1.4 in both public subnets with public addresses, ECS
# Exec on, the deployment circuit breaker rolling back, every deployment
# new-first, ignoring the deploy's revisions and the scaler's counts; a
# template naming the secrets every deployment holds alone and the optional
# ones in a label; no task and a floor of 0 until a release names the program,
# then the counts held between min and max, web and the worker on their CPU; every security group's ingress its
# program's alone; each task role ECS Exec's channels and its program's own
# permission alone (the lobby none of its own: it announces the balancer's
# address, ONNET_LOBBY_ENDPOINT; with hosting on, web's and the data program's the
# game data's store, the deployment's bucket under data/); the execution role the
# deployment's secrets and its log group alone; the fleet program named in Cloud Map.
# A size Fargate does not offer and counts out of order are refused.

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
  target          = aws_ecs_task_definition.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-fleet:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-data:1" }
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
  target          = aws_iam_role.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-fleet" }
}
override_resource {
  target          = aws_iam_role.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-worker" }
}
override_resource {
  target          = aws_iam_role.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-data" }
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

run "task_definitions" {
  command = plan

  assert {
    condition = alltrue([
      for p, d in aws_ecs_task_definition.program :
      d.family == "opennova-prod-${p}" && d.requires_compatibilities == toset(["FARGATE"]) && d.network_mode == "awsvpc" &&
      d.runtime_platform[0].cpu_architecture == "X86_64" && d.runtime_platform[0].operating_system_family == "LINUX" &&
      d.execution_role_arn == "arn:aws:iam::123456789012:role/opennova-prod-execution" &&
      d.task_role_arn == "arn:aws:iam::123456789012:role/opennova-prod-${p}"
    ]) && length(aws_ecs_task_definition.program) == 6
    error_message = "six Fargate task definitions (x86_64, awsvpc), each with the execution role and its program's own task role"
  }
  assert {
    condition = (
      aws_ecs_task_definition.program["gate"].cpu == "256" && aws_ecs_task_definition.program["gate"].memory == "512" &&
      aws_ecs_task_definition.program["lobby"].cpu == "256" && aws_ecs_task_definition.program["lobby"].memory == "512" &&
      aws_ecs_task_definition.program["fleet"].cpu == "256" && aws_ecs_task_definition.program["fleet"].memory == "512" &&
      aws_ecs_task_definition.program["web"].cpu == "512" && aws_ecs_task_definition.program["web"].memory == "1024" &&
      aws_ecs_task_definition.program["worker"].cpu == "512" && aws_ecs_task_definition.program["worker"].memory == "1024" &&
      aws_ecs_task_definition.program["data"].cpu == "256" && aws_ecs_task_definition.program["data"].memory == "1024"
    )
    error_message = "small tasks: gate, lobby and the fleet 0.25 vCPU and 0.5 GiB, web (server and nginx) and the worker 0.5 vCPU and 1 GiB, data 0.25 vCPU and 1 GiB"
  }
  assert {
    condition = alltrue([
      for p, d in aws_ecs_task_definition.program :
      jsondecode(d.container_definitions)[0].name == p &&
      jsondecode(d.container_definitions)[0].image == "ghcr.io/opennova-net/opennova-novaworld-${p}:latest" &&
      jsondecode(d.container_definitions)[0].essential &&
      jsondecode(d.container_definitions)[0].linuxParameters.initProcessEnabled &&
      jsondecode(d.container_definitions)[0].logConfiguration.logDriver == "awslogs" &&
      jsondecode(d.container_definitions)[0].logConfiguration.options["awslogs-group"] == "/opennova/prod/service" &&
      jsondecode(d.container_definitions)[0].logConfiguration.options["awslogs-stream-prefix"] == p
    ])
    error_message = "each program's container: its own image, an init as PID 1, its logs in the service's group under its program's prefix"
  }
  assert {
    condition = (
      jsondecode(aws_ecs_task_definition.program["gate"].container_definitions)[0].healthCheck.command == ["CMD", "curl", "-fsS", "-o", "/dev/null", "--max-time", "2", "http://127.0.0.1:8081/livez"] &&
      jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].healthCheck.command[7] == "http://127.0.0.1:8080/livez" &&
      alltrue([for p in ["lobby", "fleet", "worker", "data"] : jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].healthCheck.command[7] == "http://127.0.0.1:8081/livez"])
    )
    error_message = "each container's health check asks its /livez: web's on its HTTP port, the others' on the health port"
  }
  assert {
    condition = (
      alltrue([for p in ["gate", "lobby", "web", "fleet", "worker", "data"] : jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].healthCheck.startPeriod == 60])
    )
    error_message = "every program's health checks start counting after 60 s (its boot, the migrations among it)"
  }
  assert {
    condition = (
      toset([for m in jsondecode(aws_ecs_task_definition.program["gate"].container_definitions)[0].portMappings : "${m.protocol}/${m.containerPort}"]) == toset(["udp/7597", "tcp/8081"]) &&
      toset([for m in jsondecode(aws_ecs_task_definition.program["lobby"].container_definitions)[0].portMappings : "${m.protocol}/${m.containerPort}"]) == toset(["udp/64206", "tcp/8081"]) &&
      toset([for m in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].portMappings : "${m.protocol}/${m.containerPort}"]) == toset(["tcp/8080", "tcp/8081", "tcp/20044", "tcp/20556", "tcp/21068", "tcp/21580"]) &&
      toset([for m in jsondecode(aws_ecs_task_definition.program["fleet"].container_definitions)[0].portMappings : "${m.protocol}/${m.containerPort}"]) == toset(["tcp/8070", "tcp/8081"]) &&
      toset([for m in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].portMappings : "${m.protocol}/${m.containerPort}"]) == toset(["tcp/8081"]) &&
      toset([for m in jsondecode(aws_ecs_task_definition.program["data"].container_definitions)[0].portMappings : "${m.protocol}/${m.containerPort}"]) == toset(["tcp/8081"])
    )
    error_message = "each container's ports: the gate's UDP, the lobby's UDP, web's HTTP and the updater's four, the fleet's internal listener, and every program's health port"
  }
  assert {
    condition = (
      length(aws_ecs_task_definition.program["data"].ephemeral_storage) == 1 && aws_ecs_task_definition.program["data"].ephemeral_storage[0].size_in_gib == 40 &&
      alltrue([for p in ["gate", "lobby", "web", "fleet", "worker"] : length(aws_ecs_task_definition.program[p].ephemeral_storage) == 0])
    )
    error_message = "the data program's task has 40 GiB of storage (a tree's files and a release's package pass through it); the others, the worker among them, keep Fargate's default"
  }
  assert {
    condition = (
      alltrue([for p in ["gate", "lobby", "web", "fleet", "worker", "data"] : jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].stopTimeout == 30])
    )
    error_message = "every program's orderly shutdown gets 30 s (the lobby's drain among them: the balancer drained its task before the signal)"
  }
  assert {
    condition = (
      length(jsondecode(aws_ecs_task_definition.program["web"].container_definitions)) == 2 &&
      jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[1].name == "portal" &&
      jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[1].image == "ghcr.io/opennova-net/novaworld-web:latest" &&
      [for m in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[1].portMappings : "${m.protocol}/${m.containerPort}"] == ["tcp/443", "tcp/8082"] &&
      jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[1].dependsOn == [{ containerName = "web", condition = "HEALTHY" }] &&
      jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[1].logConfiguration.options["awslogs-stream-prefix"] == "web" &&
      toset([for s in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[1].secrets : s.valueFrom]) == toset([
        "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/secret/EDGE_AUTH_SECRET",
        "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/tls/ORIGIN_TLS_CERT",
        "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/tls/ORIGIN_TLS_KEY",
      ]) &&
      jsondecode(jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[1].dockerLabels["opennova.optional-secrets"]) == {
        EDGE_AUTH_SECRET_PREVIOUS = "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/secret/EDGE_AUTH_SECRET_PREVIOUS"
      } &&
      alltrue([for p in ["gate", "lobby", "fleet", "worker", "data"] : length(jsondecode(aws_ecs_task_definition.program[p].container_definitions)) == 1])
    )
    error_message = "web's task carries the portal's nginx on 443 and its retail server on 8082, started once the server answers, with edge authentication and the origin certificate (the previous edge secret optional, in its label); no other task carries it"
  }
}

run "environments" {
  command = plan

  assert {
    condition = alltrue([
      for p in ["gate", "lobby", "web", "fleet", "worker"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HEALTH_PORT"] == "8081" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["AWS_REGION"] == "us-east-1" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_GATE_UDP_PORT"] == "7597" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_NW_UDP_PORT"] == "64206" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HTTP_PORT"] == "8080"
    ])
    error_message = "every program: its region, its health port, the ports the programs listen on and announce"
  }
  assert {
    condition = (
      { for e in jsondecode(aws_ecs_task_definition.program["data"].container_definitions)[0].environment : e.name => e.value }["ONNET_HEALTH_PORT"] == "8081" &&
      { for e in jsondecode(aws_ecs_task_definition.program["data"].container_definitions)[0].environment : e.name => e.value }["AWS_REGION"] == "us-east-1" &&
      !strcontains(aws_ecs_task_definition.program["data"].container_definitions, "ONNET_PUBLIC_HOST") &&
      !strcontains(aws_ecs_task_definition.program["data"].container_definitions, "ONNET_HOSTED") &&
      !strcontains(aws_ecs_task_definition.program["data"].container_definitions, "ONNET_MAIL") &&
      !strcontains(aws_ecs_task_definition.program["data"].container_definitions, "STRIPE_")
    )
    error_message = "the data program reads its region, its health port and the database alone (the store with hosting on): nothing of the fleet, the mail or billing"
  }
  assert {
    condition = alltrue([
      for p in ["gate", "lobby", "web", "fleet", "worker", "data"] :
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_ROLE") &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_INTERNAL_HTTP_BIND")
    ])
    error_message = "no program is told a role; the fleet finds its bind address from the task metadata"
  }
  assert {
    condition = (
      { for e in jsondecode(aws_ecs_task_definition.program["lobby"].container_definitions)[0].environment : e.name => e.value }["ONNET_LOBBY_ENDPOINT"] == "54.162.79.147:64206" &&
      { for e in jsondecode(aws_ecs_task_definition.program["lobby"].container_definitions)[0].environment : e.name => e.value }["ONNET_PUBLIC_HOST"] == "54.162.79.147" &&
      alltrue([for p in ["gate", "web", "fleet", "worker", "data"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_LOBBY_ENDPOINT")])
    )
    error_message = "every lobby announces the one lobby address, the gate's (the balancer's zone-a Elastic IP) and its port; no other program is told it (the gate's would be its fallback, answering while no lobby is live)"
  }
  assert {
    condition = (
      toset([for s in jsondecode(aws_ecs_task_definition.program["gate"].container_definitions)[0].secrets : s.name]) == toset(["DATABASE_PASSWORD"]) &&
      toset([for s in jsondecode(aws_ecs_task_definition.program["lobby"].container_definitions)[0].secrets : s.name]) == toset(["DATABASE_PASSWORD"]) &&
      !strcontains(aws_ecs_task_definition.program["gate"].container_definitions, "ONNET_HOSTED") &&
      !strcontains(aws_ecs_task_definition.program["lobby"].container_definitions, "ONNET_HOSTED") &&
      !strcontains(aws_ecs_task_definition.program["gate"].container_definitions, "ONNET_MAIL")
    )
    error_message = "the gate and the lobby read the database's password alone, and nothing of hosting or mail"
  }
  assert {
    condition = (
      toset([for s in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].secrets : s.name]) == toset(["DATABASE_PASSWORD", "ADMIN_API_TOKEN", "ONNET_REDIS_PASSWORD"]) &&
      toset([for s in jsondecode(aws_ecs_task_definition.program["fleet"].container_definitions)[0].secrets : s.name]) == toset(["DATABASE_PASSWORD"]) &&
      toset([for s in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].secrets : s.name]) == toset(["DATABASE_PASSWORD"]) &&
      toset([for s in jsondecode(aws_ecs_task_definition.program["data"].container_definitions)[0].secrets : s.name]) == toset(["DATABASE_PASSWORD"]) &&
      alltrue([for p, d in aws_ecs_task_definition.program : alltrue([for s in jsondecode(d.container_definitions)[0].secrets : s.valueFrom == "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/secret/${s.name}"])])
    )
    error_message = "a template names the secrets every deployment holds alone: the database's password, and web's admin token and cache token"
  }
  assert {
    condition = (
      toset(keys(jsondecode(jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].dockerLabels["opennova.optional-secrets"]))) == toset([
        "ONNET_BOOTSTRAP_ADMIN", "STRIPE_SECRET_KEY", "STRIPE_WEBHOOK_SECRET", "STRIPE_PRICE_TIER1", "STRIPE_PRICE_TIER2", "STRIPE_PRICE_TIER3", "STRIPE_PRICE_OWN_MACHINE",
        "STRIPE_PRICE_STATIC_ADDRESS",
      ]) &&
      toset(keys(jsondecode(jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].dockerLabels["opennova.optional-secrets"]))) == toset([
        "STRIPE_SECRET_KEY", "STRIPE_WEBHOOK_SECRET", "STRIPE_PRICE_TIER1", "STRIPE_PRICE_TIER2", "STRIPE_PRICE_TIER3", "STRIPE_PRICE_OWN_MACHINE",
        "STRIPE_PRICE_STATIC_ADDRESS",
      ]) &&
      alltrue([for p in ["gate", "lobby", "fleet", "data"] : jsondecode(jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].dockerLabels["opennova.optional-secrets"]) == {}]) &&
      jsondecode(jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].dockerLabels["opennova.optional-secrets"])["STRIPE_SECRET_KEY"] == "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/secret/STRIPE_SECRET_KEY"
    )
    error_message = "the optional secrets (the first admin; billing's, for web and the worker) are in each template's label, for a deploy to add while SSM holds them; the fleet and the data program read none"
  }
  assert {
    condition = (
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_TRUSTED_PROXIES"] == "127.0.0.1" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_API_PROXY_ONLY"] == "1" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_RETAIL_PROXY"] == "127.0.0.2" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_COOKIE_SECURE"] == "1" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_SITE_URL"] == "https://example.com" &&
      !strcontains(aws_ecs_task_definition.program["worker"].container_definitions, "ONNET_TRUSTED_PROXIES")
    )
    error_message = "web trusts its own nginx on the task's loopback, answers /api/ to it alone, takes the client's address from the retail server (127.0.0.2), sets a Secure cookie on the https site"
  }
  assert {
    condition = (
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_UPDATE_HOST"] == "update.example.com" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_UPDATE_PORTS"] == "20044,20556,21068,21580" &&
      alltrue([for p in ["gate", "lobby", "fleet", "worker", "data"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_UPDATE_")])
    )
    error_message = "web alone serves the update service: at the update record's name, on the updater's four ports"
  }
  assert {
    condition = (alltrue([
      for p in ["web", "fleet", "worker"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_PROVIDER"] == "none" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_FLEET_SHARED_SLOTS"] == "6" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_CAP"] == "14"
      ]) && alltrue([for p in ["web", "worker"] : { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_SITE_URL"] == "https://example.com"]) &&
    alltrue([for p in ["gate", "lobby", "web", "fleet", "worker", "data"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_INTERNAL_URL")]))
    error_message = "web, the fleet and the worker read the fleet's policy (hosting off without a data set); no program is handed the machines' URL without hosting"
  }
  assert {
    condition = (
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_REGION"] == "us-east-1" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_CLUSTER"] == "opennova-prod" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_FLEET_CLUSTER"] == "" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_DATABASE"] == "opennova-prod-db" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_CACHE"] == "opennova-prod-cache" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_RELEASE_PARAMETER"] == "/opennova/prod/release" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_LOAD_BALANCER"] == "net/opennova-prod/0123456789abcdef" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_ALARM_PREFIX"] == "opennova-prod-" &&
      { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_LOG_STORE"] == "cloudwatch:/opennova/prod" &&
      jsondecode({ for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_TARGET_GROUPS"]) == {
        gate   = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-gate/0123456789abcdef"
        lobby  = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-mock/0123456789abcdef"
        http   = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-http/0123456789abcdef"
        https  = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-https/0123456789abcdef"
        update = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-mock/0123456789abcdef"
      } &&
      alltrue([for p in ["gate", "lobby", "fleet", "worker", "data"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_INFRA_")]) &&
      alltrue([for p in ["gate", "lobby", "fleet", "worker", "data"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_LOG_STORE")])
    )
    error_message = "web alone is told what the admin's infrastructure view reads: this deployment's cluster (and no fleet cluster with hosting off), its target groups by listener, its database, cache, balancer and release parameter, in its region, and its log groups' prefix"
  }
}

run "services" {
  command = plan

  assert {
    condition = alltrue([
      for p, s in aws_ecs_service.program :
      s.name == p && s.launch_type == "FARGATE" && s.platform_version == "1.4.0" && s.enable_execute_command &&
      s.propagate_tags == "SERVICE" && s.enable_ecs_managed_tags && s.desired_count == 0 &&
      s.deployment_circuit_breaker[0].enable && s.deployment_circuit_breaker[0].rollback &&
      toset(s.network_configuration[0].subnets) == toset(["subnet-0public0a", "subnet-0public0b"]) &&
      s.network_configuration[0].security_groups == toset(["sg-0${p}"]) && s.network_configuration[0].assign_public_ip
    ]) && length(aws_ecs_service.program) == 6
    error_message = "six services on Fargate 1.4 in both public subnets, each task on a public address in its program's group, ECS Exec on, the circuit breaker rolling back, and no task before the first release"
  }
  assert {
    condition = (
      alltrue([for p, s in aws_ecs_service.program : s.deployment_minimum_healthy_percent == 100 && s.deployment_maximum_percent == 200])
    )
    error_message = "every program starts the new task first (the fleet's waits as standby; the balancer drains the old lobby task before ECS stops it): a bad image stops nothing that runs"
  }
  assert {
    condition = (
      length(aws_ecs_service.program["fleet"].service_registries) == 1 &&
      aws_ecs_service.program["fleet"].service_registries[0].registry_arn == "arn:aws:servicediscovery:us-east-1:123456789012:service/srv-0123456789abcdef" &&
      alltrue([for p in ["gate", "lobby", "web", "worker", "data"] : length(aws_ecs_service.program[p].service_registries) == 0])
    )
    error_message = "the fleet program alone registers in Cloud Map"
  }
  assert {
    condition = (
      aws_ecs_cluster.main.name == "opennova-prod" && output.cluster == "opennova-prod" &&
      output.services == { gate = "gate", lobby = "lobby", web = "web", fleet = "fleet", worker = "worker", data = "data" }
    )
    error_message = "the cluster and its services, named after the programs"
  }
}

run "task_counts" {
  command = plan

  assert {
    condition = alltrue([
      for p, t in aws_appautoscaling_target.program :
      t.service_namespace == "ecs" && t.scalable_dimension == "ecs:service:DesiredCount" &&
      t.resource_id == "service/opennova-prod/${p}" && t.min_capacity == 0
    ]) && length(aws_appautoscaling_target.program) == 6
    error_message = "every service's count is held under its program's max, its floor 0 until the first release"
  }
  assert {
    condition = (
      aws_appautoscaling_target.program["web"].max_capacity == 1 && aws_appautoscaling_target.program["worker"].max_capacity == 1 &&
      aws_appautoscaling_target.program["gate"].max_capacity == 1 && aws_appautoscaling_target.program["lobby"].max_capacity == 1 &&
      aws_appautoscaling_target.program["fleet"].max_capacity == 1 && aws_appautoscaling_target.program["data"].max_capacity == 1
    )
    error_message = "every program one task at the defaults"
  }
  assert {
    condition = (
      toset(keys(aws_appautoscaling_policy.cpu)) == toset(["web", "worker"]) &&
      alltrue([for p, a in aws_appautoscaling_policy.cpu :
        a.policy_type == "TargetTrackingScaling" &&
        a.target_tracking_scaling_policy_configuration[0].target_value == 60 &&
        a.target_tracking_scaling_policy_configuration[0].predefined_metric_specification[0].predefined_metric_type == "ECSServiceAverageCPUUtilization"
      ])
    )
    error_message = "web and the worker scale on their CPU (60% on average), no other program (the data program never)"
  }
}

run "security_groups" {
  command = plan

  assert {
    condition = (
      toset(keys(aws_security_group.program)) == toset(["gate", "lobby", "web", "fleet", "worker", "data"]) &&
      alltrue([for p, g in aws_security_group.program : g.name == "opennova-prod-${p}"]) &&
      alltrue([for p, r in aws_vpc_security_group_egress_rule.program_all : r.security_group_id == "sg-0${p}" && r.cidr_ipv4 == "0.0.0.0/0" && r.ip_protocol == "-1"])
    )
    error_message = "a security group per program, each reaching out anywhere (the AWS APIs, GHCR, Stripe through its public address)"
  }
  assert {
    condition = (
      length(aws_vpc_security_group_ingress_rule.lobby) == 1 &&
      aws_vpc_security_group_ingress_rule.lobby["0.0.0.0/0"].security_group_id == "sg-0lobby" &&
      aws_vpc_security_group_ingress_rule.lobby["0.0.0.0/0"].ip_protocol == "udp" &&
      aws_vpc_security_group_ingress_rule.lobby["0.0.0.0/0"].from_port == 64206 && aws_vpc_security_group_ingress_rule.lobby["0.0.0.0/0"].to_port == 64206
    )
    error_message = "the lobby admits its UDP port from everyone, on its own address as well as through the balancer"
  }
  assert {
    condition = alltrue(concat(
      [for r in aws_vpc_security_group_ingress_rule.from_lb : contains(["sg-0gate", "sg-0lobby", "sg-0web"], r.security_group_id)],
      [length(aws_vpc_security_group_ingress_rule.fleet_from_machines) == 0],
    ))
    error_message = "the worker and the data program admit nothing, the fleet nothing without hosting (fleet.tf adds the machines), the gate and web the balancer alone, the lobby the balancer and its UDP port"
  }
}

run "task_roles" {
  command = plan

  assert {
    condition = (
      alltrue([for p, r in aws_iam_role.program : r.name == "opennova-prod-${p}" && r.permissions_boundary == "arn:aws:iam::123456789012:policy/opennova-role-boundary"]) &&
      aws_iam_role.execution.permissions_boundary == "arn:aws:iam::123456789012:policy/opennova-role-boundary" &&
      length([
        for st in data.aws_iam_policy_document.ecs_tasks_assume_role.statement : st
        if st.actions == toset(["sts:AssumeRole"]) && one(st.principals).identifiers == toset(["ecs-tasks.amazonaws.com"]) &&
        length([for c in st.condition : c if c.variable == "aws:SourceAccount" && toset(c.values) == toset(["123456789012"])]) == 1
      ]) == 1
    )
    error_message = "a task role per program and the execution role, each under the role boundary, assumed by this account's ECS tasks alone"
  }
  assert {
    condition = (
      toset(keys(aws_iam_role_policy.exec)) == toset(["gate", "lobby", "web", "fleet", "worker", "data"]) &&
      alltrue([for p, r in aws_iam_role_policy.exec : r.role == aws_iam_role.program[p].id]) &&
      toset(flatten([for st in data.aws_iam_policy_document.exec.statement : st.actions])) == toset([
        "ssmmessages:CreateControlChannel", "ssmmessages:CreateDataChannel", "ssmmessages:OpenControlChannel", "ssmmessages:OpenDataChannel",
      ])
    )
    error_message = "every task role holds ECS Exec's four channel actions"
  }
  assert {
    condition = (
      length(aws_iam_role_policy.fleet_service) == 0 && length(aws_iam_role_policy.mail) == 0 && length(aws_iam_role_policy.data_store) == 0
    )
    error_message = "without hosting and mail, every task role holds ECS Exec alone but web's infrastructure reads (fleet.tf and mail.tf add the worker's, hosting the store's)"
  }
  assert {
    condition = (
      aws_iam_role_policy.infra_read.role == aws_iam_role.program["web"].id &&
      aws_iam_role_policy.infra_read.name == "opennova-prod-web-infra-read" &&
      toset(flatten([for st in data.aws_iam_policy_document.infra_read.statement : st.actions])) == toset([
        "ecs:DescribeServices", "ecs:ListTasks", "ecs:DescribeTasks", "elasticloadbalancing:DescribeTargetHealth",
        "rds:DescribeDBInstances", "elasticache:DescribeReplicationGroups", "ssm:GetParameter", "ec2:DescribeInstances",
        "logs:GetLogEvents", "logs:FilterLogEvents", "logs:DescribeLogStreams", "cloudwatch:GetMetricData",
        "cloudwatch:DescribeAlarms",
      ]) &&
      alltrue([for st in data.aws_iam_policy_document.infra_read.statement : !anytrue([for a in st.actions : !can(regex("^[a-z0-9]+:(Describe|List|Get|Filter)", a))])])
    )
    error_message = "web's role holds the infrastructure view's reads, and nothing but reads"
  }
  assert {
    condition = (
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadLogs"]) == toset(flatten([
        for g in ["service", "machines", "servers"] : [
          "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/${g}",
          "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/${g}:*",
        ]
      ])) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadMetrics"]) == toset(["*"]) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadAlarms"]) == toset(["arn:aws:cloudwatch:us-east-1:123456789012:alarm:opennova-prod-*"])
    )
    error_message = "web reads the events of this deployment's three log groups alone (the programs', the machines', the servers'), CloudWatch's metrics, which take no resource, and this deployment's alarms alone"
  }
  assert {
    condition = (
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadServices"]) == toset(["arn:aws:ecs:us-east-1:123456789012:service/opennova-prod/*"]) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "DescribeTasks"]) == toset(["arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/*"]) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st if st.sid == "ListTasks"]).resources == toset(["*"]) &&
      length([for c in one([for st in data.aws_iam_policy_document.infra_read.statement : st if st.sid == "ListTasks"]).condition : c
      if c.test == "ArnEquals" && c.variable == "ecs:cluster" && toset(c.values) == toset(["arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod"])]) == 1 &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadDatabase"]) == toset([aws_db_instance.main.arn]) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadCache"]) == toset([aws_elasticache_replication_group.cache.arn]) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadRelease"]) == toset([aws_ssm_parameter.release.arn]) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadTargetHealth"]) == toset(["*"]) &&
      one([for st in data.aws_iam_policy_document.infra_read.statement : st.resources if st.sid == "ReadMachines"]) == toset(["*"])
    )
    error_message = "each infrastructure read names this deployment's own cluster, tasks, database, cache and release parameter; ListTasks its cluster as a condition; the balancer's and EC2's describes take no resource"
  }
  assert {
    condition = (
      toset(flatten([for st in data.aws_iam_policy_document.execution.statement : st.actions])) == toset(["ssm:GetParameters", "logs:CreateLogStream", "logs:PutLogEvents"]) &&
      length([for st in data.aws_iam_policy_document.execution.statement : st if st.sid == "ReadTaskSecrets" && toset(st.resources) == toset([
        "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/secret/*",
        "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/tls/*",
      ])]) == 1 &&
      aws_iam_role_policy.execution.role == aws_iam_role.execution.id
    )
    error_message = "the execution role reads the deployment's secrets and origin certificate and ships the logs, nothing else (the images are public)"
  }
}

# Hosting on: web and the data program get the game data's store, the
# deployment's bucket under data/ alone, and the others nothing of it; the
# fleet's policy goes to web, the fleet and the worker, the machine driver's own
# settings to the worker alone, and the fleet's role holds ECS Exec alone.
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
    condition = (
      toset(keys(aws_iam_role_policy.data_store)) == toset(["web", "data"]) &&
      alltrue([for p, r in aws_iam_role_policy.data_store : r.role == aws_iam_role.program[p].id && r.name == "opennova-prod-${p}-data-store"]) &&
      aws_iam_role_policy.fleet_service[0].role == aws_iam_role.program["worker"].id
    )
    error_message = "web's and the data program's task roles hold the store's policy, no other's (the worker holds the fleet's EC2 calls)"
  }
  assert {
    condition = (
      length([
        for st in data.aws_iam_policy_document.data_store[0].statement : st
        if st.sid == "ListDataStore" && toset(st.actions) == toset(["s3:ListBucket"]) && toset(st.resources) == toset(["arn:aws:s3:::opennova-prod-ops-123456789012"]) &&
        length([for c in st.condition : c if c.test == "StringLike" && c.variable == "s3:prefix" && toset(c.values) == toset(["data/*"])]) == 1
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.data_store[0].statement : st
        if st.sid == "ListDataStoreUploads" && toset(st.actions) == toset(["s3:ListBucketMultipartUploads"]) && toset(st.resources) == toset(["arn:aws:s3:::opennova-prod-ops-123456789012"]) && length(st.condition) == 0
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.data_store[0].statement : st
        if st.sid == "DataStoreObjects" && toset(st.resources) == toset(["arn:aws:s3:::opennova-prod-ops-123456789012/data/*"]) &&
        toset(st.actions) == toset(["s3:GetObject", "s3:PutObject", "s3:DeleteObject", "s3:AbortMultipartUpload"])
      ]) == 1 &&
      length(data.aws_iam_policy_document.data_store[0].statement) == 3
    )
    error_message = "the store's policy: list data/ and the uploads in flight; get, put, delete and abort under data/ alone"
  }
  assert {
    condition = alltrue([
      for p in ["web", "fleet", "worker", "data"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_OBJECT_STORE_URL"] == "s3://opennova-prod-ops-123456789012/data" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_OBJECT_STORE_REGION"] == "us-east-1"
    ]) && alltrue([for p in ["gate", "lobby"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_OBJECT_STORE")])
    error_message = "web, the fleet, the worker and the data program name the store; the gate and the lobby know nothing of it"
  }
  assert {
    condition = (
      alltrue([for p in ["web", "fleet", "worker"] :
        { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_PROVIDER"] == "ec2" &&
        { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_FLEET_MAX_SHARED"] == "2"
      ]) &&
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_INTERNAL_URL"] == "http://fleet.prod.opennova.internal:8070" &&
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_EC2_LAUNCH_TEMPLATES"] == jsonencode({ own = "lt-0own", shared = "lt-0shared" }) &&
      alltrue([for p in ["gate", "lobby", "web", "fleet", "data"] :
        !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_EC2_") &&
        !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_INTERNAL_URL") &&
        !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_DEPLOYMENT")
      ]) &&
      !strcontains(aws_ecs_task_definition.program["data"].container_definitions, "ONNET_HOSTED_PROVIDER")
    )
    error_message = "the fleet's policy for web, the fleet and the worker; the machine driver's own settings (the templates, the subnets, the machines' URL) for the worker alone"
  }
}

run "cloud_map" {
  command = plan

  assert {
    condition = (
      aws_service_discovery_private_dns_namespace.internal.name == "prod.opennova.internal" &&
      aws_service_discovery_service.fleet.name == "fleet" && aws_service_discovery_service.fleet.force_destroy &&
      aws_service_discovery_service.fleet.dns_config[0].namespace_id == "ns-0123456789abcdef" &&
      aws_service_discovery_service.fleet.dns_config[0].routing_policy == "MULTIVALUE" &&
      aws_service_discovery_service.fleet.dns_config[0].dns_records[0].type == "A" &&
      aws_service_discovery_service.fleet.dns_config[0].dns_records[0].ttl == 10 &&
      output.fleet_host == "fleet.prod.opennova.internal" && output.internal_url == "http://fleet.prod.opennova.internal:8070"
    )
    error_message = "the fleet's tasks register as fleet.prod.opennova.internal, an A record with a 10 s TTL, the name hostd and ONNET_INTERNAL_URL use, its instances removed with it"
  }
}

# Once a release is recorded (app deploy writes its manifest over "none"), a
# new service starts at its desired count and every count's floor is its min.
run "released" {
  command = plan

  variables {
    lobby                = { min = 2, max = 2, desired = 2 }
    db_service_processes = 16
  }

  override_data {
    target          = data.aws_ssm_parameters_by_path.deployment
    override_during = plan
    values = {
      names  = ["/opennova/prod/infra.env", "/opennova/prod/release"]
      values = ["OPENNOVA_DEPLOYMENT='prod'", "{\"id\":\"20261016T000000Z-abc1234\",\"programs\":[\"worker\",\"data\",\"fleet\",\"web\",\"lobby\",\"gate\"]}"]
    }
  }

  assert {
    condition = (
      alltrue([for p, s in aws_ecs_service.program : s.desired_count >= 1]) &&
      aws_ecs_service.program["lobby"].desired_count == 2 && aws_ecs_service.program["web"].desired_count == 1 &&
      aws_appautoscaling_target.program["lobby"].min_capacity == 2 && aws_appautoscaling_target.program["web"].min_capacity == 1 &&
      aws_cloudwatch_metric_alarm.program_running["lobby"].threshold == 2 &&
      aws_ecs_service.program["lobby"].deployment_minimum_healthy_percent == 100 && aws_ecs_service.program["lobby"].deployment_maximum_percent == 200
    )
    error_message = "released: each service starts at its desired count, its floor its min, and fewer running than its min alarms; two lobbies roll new-first too"
  }
}

# At the cutover the parameter holds the single service instance's release id,
# which names no program: the services still start with no task.
run "single_instance_release" {
  command = plan

  override_data {
    target          = data.aws_ssm_parameters_by_path.deployment
    override_during = plan
    values = {
      names  = ["/opennova/prod/release"]
      values = ["20261015T120000Z-abcdef1"]
    }
  }

  assert {
    condition     = alltrue([for p, s in aws_ecs_service.program : s.desired_count == 0 && aws_appautoscaling_target.program[p].min_capacity == 0])
    error_message = "a release of the single instance is no release of the programs: no task starts from a template"
  }
}

# A release from before a program was added (here the controller's era: no
# fleet, no data) starts the programs it names alone: a program no release names
# yet runs no task, its floor 0 and its heartbeat and running alarms off.
run "released_per_program" {
  command = plan

  override_data {
    target          = data.aws_ssm_parameters_by_path.deployment
    override_during = plan
    values = {
      names  = ["/opennova/prod/infra.env", "/opennova/prod/release"]
      values = ["OPENNOVA_DEPLOYMENT='prod'", "{\"id\":\"20261016T000000Z-abc1234\",\"programs\":[\"worker\",\"controller\",\"web\",\"lobby\",\"gate\"]}"]
    }
  }

  assert {
    condition = (
      alltrue([for p in ["gate", "lobby", "web", "worker"] :
        aws_ecs_service.program[p].desired_count == 1 && aws_appautoscaling_target.program[p].min_capacity == 1 &&
        aws_cloudwatch_metric_alarm.program_heartbeat[p].treat_missing_data == "breaching" &&
        aws_cloudwatch_metric_alarm.program_running[p].threshold == 1
      ]) &&
      alltrue([for p in ["fleet", "data"] :
        aws_ecs_service.program[p].desired_count == 0 && aws_appautoscaling_target.program[p].min_capacity == 0 &&
        aws_cloudwatch_metric_alarm.program_heartbeat[p].treat_missing_data == "notBreaching" &&
        aws_cloudwatch_metric_alarm.program_running[p].threshold == 0 &&
        aws_cloudwatch_metric_alarm.program_running[p].treat_missing_data == "notBreaching"
      ]) &&
      aws_scheduler_schedule.backup.state == "ENABLED"
    )
    error_message = "a program the release names runs and alarms; one it does not name (a newer service) has no task, a floor of 0 and its alarms off; the backup runs with any release"
  }
}

# A field left out keeps its default.
run "web_sizes" {
  command = plan

  variables {
    web                  = { max = 6 }
    db_service_processes = 25
  }

  assert {
    condition = (
      aws_appautoscaling_target.program["web"].max_capacity == 6 && aws_appautoscaling_target.program["web"].min_capacity == 0 &&
      aws_ecs_task_definition.program["web"].cpu == "512" && aws_ecs_task_definition.program["web"].memory == "1024"
    )
    error_message = "web = { max = 6 } keeps the other fields' defaults"
  }
}

run "no_fargate_size_refused" {
  command = plan

  variables {
    gate = { cpu = 256, memory = 4096 }
  }

  expect_failures = [aws_ecs_task_definition.program]
}

run "counts_out_of_order_refused" {
  command = plan

  variables {
    web = { min = 3, desired = 2 }
  }

  expect_failures = [aws_ecs_service.program]
}
