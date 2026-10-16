# The public network load balancer (load_balancer.tf), planned against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare. From
# the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# An internet-facing network load balancer across both zones, cross-zone,
# zone a on the gate's (adopted) Elastic IP and zone b on one of its own; UDP
# 7597 to the gate's tasks, UDP 64206 to the lobby's, TCP 443 to web's portal
# and the retail port to web's server, IP targets that see each client's own
# address; health on the gate's and the lobby's /livez (their health port),
# 443's handshake and /healthz; the UDP flows ended with the deregistration
# delay, the lobby's 10 s; the gate, the lobby and the retail port from
# everyone, 443 from Cloudflare's ranges alone; the balancer's group sends to
# the gate's, the lobby's and web's alone, and theirs admit it; the gate's,
# the lobby's and web's services register with their target groups; Route
# 53's check takes the path a stock client takes.

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
  target          = aws_lb_target_group.public["lobby"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-lobby/0123456789abcdef", arn_suffix = "targetgroup/opennova-prod-lobby/0123456789abcdef" }
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

run "load_balancer" {
  command = plan

  assert {
    condition = (
      aws_lb.public.load_balancer_type == "network" && !aws_lb.public.internal && aws_lb.public.enable_cross_zone_load_balancing &&
      aws_lb.public.name == "opennova-prod" && aws_lb.public.security_groups == toset(["sg-0lb"]) &&
      toset([for m in aws_lb.public.subnet_mapping : "${m.subnet_id}=${m.allocation_id}"]) == toset([
        "subnet-0public0a=eipalloc-0123456789abcdef0",
        "subnet-0public0b=eipalloc-0000000000000000b",
      ])
    )
    error_message = "an internet-facing network load balancer across both zones, zone a on the gate's adopted Elastic IP, zone b on one of its own, behind its own security group"
  }
  assert {
    condition = (
      aws_lb_target_group.public["gate"].protocol == "UDP" && aws_lb_target_group.public["gate"].port == 7597 &&
      aws_lb_target_group.public["gate"].health_check[0].protocol == "HTTP" && aws_lb_target_group.public["gate"].health_check[0].port == "8081" &&
      aws_lb_target_group.public["gate"].health_check[0].path == "/livez" &&
      aws_lb_target_group.public["lobby"].protocol == "UDP" && aws_lb_target_group.public["lobby"].port == 64206 &&
      aws_lb_target_group.public["lobby"].health_check[0].protocol == "HTTP" && aws_lb_target_group.public["lobby"].health_check[0].port == "8081" &&
      aws_lb_target_group.public["lobby"].health_check[0].path == "/livez" &&
      aws_lb_target_group.public["https"].protocol == "TCP" && aws_lb_target_group.public["https"].port == 443 &&
      aws_lb_target_group.public["https"].health_check[0].protocol == "TCP" &&
      aws_lb_target_group.public["http"].protocol == "TCP" && aws_lb_target_group.public["http"].port == 8080 &&
      aws_lb_target_group.public["http"].health_check[0].protocol == "HTTP" && aws_lb_target_group.public["http"].health_check[0].path == "/healthz" &&
      alltrue([for tg in aws_lb_target_group.public : tg.target_type == "ip" && tg.preserve_client_ip == "true"])
    )
    error_message = "the gate's and the lobby's UDP ports, 443 and the retail port to IP targets that see each client's own address; health: the gate's and the lobby's /livez on their health port, 443's handshake, /healthz"
  }
  assert {
    condition = (
      tostring(aws_lb_target_group.public["lobby"].deregistration_delay) == "10" &&
      alltrue([for k in ["gate", "https", "http"] : tostring(aws_lb_target_group.public[k].deregistration_delay) == "30"]) &&
      aws_lb_target_group.public["lobby"].connection_termination && aws_lb_target_group.public["gate"].connection_termination
    )
    error_message = "a deregistering lobby task keeps its flows 10 s (its sessions move by timing out anyway), the others 30 s; the UDP flows end with the delay, so no client's flow outlives its task"
  }
  assert {
    condition = (
      toset([for l in aws_lb_listener.public : "${l.port}/${l.protocol}"]) == toset(["7597/UDP", "64206/UDP", "443/TCP", "8080/TCP"]) &&
      aws_lb_listener.public["gate"].default_action[0].target_group_arn == "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-gate/0123456789abcdef" &&
      aws_lb_listener.public["lobby"].default_action[0].target_group_arn == "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-lobby/0123456789abcdef" &&
      aws_lb_listener.public["http"].default_action[0].target_group_arn == "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-http/0123456789abcdef"
    )
    error_message = "a listener per port, each forwarding to its target group (no TLS ends at the balancer)"
  }
  assert {
    condition = (
      length([for r in aws_vpc_security_group_ingress_rule.lb : r if r.from_port == 443]) == 2 &&
      alltrue([for r in aws_vpc_security_group_ingress_rule.lb : contains(["173.245.48.0/20", "103.21.244.0/22"], r.cidr_ipv4) if r.from_port == 443]) &&
      length([for r in aws_vpc_security_group_ingress_rule.lb : r if r.from_port == 8080 && r.ip_protocol == "tcp" && r.cidr_ipv4 == "0.0.0.0/0"]) == 1 &&
      length([for r in aws_vpc_security_group_ingress_rule.lb : r if r.from_port == 7597 && r.ip_protocol == "udp" && r.cidr_ipv4 == "0.0.0.0/0"]) == 1 &&
      length([for r in aws_vpc_security_group_ingress_rule.lb : r if r.from_port == 64206 && r.ip_protocol == "udp" && r.cidr_ipv4 == "0.0.0.0/0"]) == 1 &&
      length(aws_vpc_security_group_ingress_rule.lb) == 5
    )
    error_message = "the balancer admits the gate's and the lobby's UDP ports and the retail port from everyone, 443 from Cloudflare's ranges alone"
  }
  assert {
    condition = (
      toset([for r in aws_vpc_security_group_egress_rule.lb : "${r.referenced_security_group_id} ${r.ip_protocol}/${r.from_port}"]) ==
      toset(["sg-0gate udp/7597", "sg-0gate tcp/8081", "sg-0lobby udp/64206", "sg-0lobby tcp/8081", "sg-0web tcp/443", "sg-0web tcp/8080"])
    )
    error_message = "the balancer sends to the gate and the lobby (each its UDP port and its health port) and web (443, the retail port) alone"
  }
  assert {
    condition = (
      toset([for r in aws_vpc_security_group_ingress_rule.from_lb : "${r.security_group_id} ${r.ip_protocol}/${r.from_port}"]) ==
      toset(["sg-0gate udp/7597", "sg-0gate tcp/8081", "sg-0lobby udp/64206", "sg-0lobby tcp/8081", "sg-0web tcp/443", "sg-0web tcp/8080"]) &&
      alltrue([for r in aws_vpc_security_group_ingress_rule.from_lb : r.referenced_security_group_id == "sg-0lb" && r.cidr_ipv4 == null])
    )
    error_message = "the gate, the lobby and web admit the balancer's group on their target ports and health ports"
  }
  assert {
    condition = (
      toset([for l in aws_ecs_service.program["gate"].load_balancer : "${l.container_name}:${l.container_port}=${l.target_group_arn}"]) == toset([
        "gate:7597=arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-gate/0123456789abcdef",
      ]) &&
      toset([for l in aws_ecs_service.program["lobby"].load_balancer : "${l.container_name}:${l.container_port}=${l.target_group_arn}"]) == toset([
        "lobby:64206=arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-lobby/0123456789abcdef",
      ]) &&
      toset([for l in aws_ecs_service.program["web"].load_balancer : "${l.container_name}:${l.container_port}=${l.target_group_arn}"]) == toset([
        "portal:443=arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-https/0123456789abcdef",
        "web:8080=arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-http/0123456789abcdef",
      ]) &&
      alltrue([for p in ["controller", "worker"] : length(aws_ecs_service.program[p].load_balancer) == 0]) &&
      alltrue([for p in ["gate", "lobby", "web"] : aws_ecs_service.program[p].health_check_grace_period_seconds == 120]) &&
      aws_ecs_service.program["lobby"].network_configuration[0].assign_public_ip &&
      aws_lb.public.enable_deletion_protection
    )
    error_message = "the gate's service registers its gate container on UDP 7597, the lobby's its lobby container on UDP 64206, web's its portal on 443 and its server on the retail port; no other program is behind the balancer, which deletion protection keeps"
  }
  assert {
    condition = (
      output.lb_dns_name == "opennova-prod-0123456789abcdef.elb.us-east-1.amazonaws.com" &&
      output.lb_arn == "arn:aws:elasticloadbalancing:us-east-1:123456789012:loadbalancer/net/opennova-prod/0123456789abcdef" &&
      output.lb_addresses == { a = "54.162.79.147", b = "5.5.5.5" }
    )
    error_message = "the outputs name the balancer and its two addresses, zone a's the gate's"
  }
  assert {
    condition = (
      aws_route53_health_check.service.ip_address == "54.162.79.147" && aws_route53_health_check.service.port == 8080 &&
      aws_route53_health_check.service.resource_path == "/healthz" && aws_route53_health_check.service.type == "HTTP"
    )
    error_message = "Route 53 checks /healthz on the retail port at the gate's address"
  }
}

# Without Cloudflare there is no origin certificate (nginx listens on 80):
# no 443 listener, target group or rule, and web registers its server alone.
run "no_cloudflare" {
  command = plan

  variables {
    cloudflare_zone_id = ""
    cloudflare_domain  = ""
  }

  assert {
    condition = (
      length(aws_vpc_security_group_ingress_rule.lb) == 3 && length([for r in aws_vpc_security_group_ingress_rule.lb : r if r.from_port == 443]) == 0 &&
      toset(keys(aws_lb_listener.public)) == toset(["gate", "lobby", "http"]) && toset(keys(aws_lb_target_group.public)) == toset(["gate", "lobby", "http"]) &&
      length([for r in aws_vpc_security_group_egress_rule.lb : r if r.from_port == 443]) == 0 &&
      length([for r in aws_vpc_security_group_ingress_rule.from_lb : r if r.from_port == 443]) == 0 &&
      toset([for l in aws_ecs_service.program["web"].load_balancer : l.container_name]) == toset(["web"]) &&
      !strcontains(aws_ecs_task_definition.program["web"].container_definitions, "ORIGIN_TLS_CERT")
    )
    error_message = "without Cloudflare: no 443 anywhere, the gate's, the lobby's and the retail port's rules alone, and no origin certificate for the portal"
  }
}

# Records not proxied: the same, with the zone.
run "unproxied" {
  command = plan

  variables {
    cloudflare_web_proxied = false
  }

  assert {
    condition = (
      toset(keys(aws_lb_listener.public)) == toset(["gate", "lobby", "http"]) && toset(keys(aws_lb_target_group.public)) == toset(["gate", "lobby", "http"]) &&
      length([for r in aws_vpc_security_group_ingress_rule.lb : r if r.from_port == 443]) == 0 &&
      toset(keys(aws_cloudwatch_metric_alarm.lb_unhealthy)) == toset(["gate", "lobby", "http"])
    )
    error_message = "unproxied records: no 443 listener, target group, rule or alarm"
  }
}

# Two allowed ranges: a rule each on the gate's, the lobby's and the retail
# port at the balancer, and on the lobby's own address.
run "allowed_cidrs" {
  command = plan

  variables {
    allowed_cidrs = ["198.51.100.0/24", "203.0.113.0/24"]
  }

  assert {
    condition = (
      length([for r in aws_vpc_security_group_ingress_rule.lb : r if r.from_port != 443]) == 6 &&
      toset([for r in aws_vpc_security_group_ingress_rule.lobby : r.cidr_ipv4]) == toset(["198.51.100.0/24", "203.0.113.0/24"])
    )
    error_message = "allowed_cidrs bounds the gate's, the retail port's and the lobby's rules"
  }
}
