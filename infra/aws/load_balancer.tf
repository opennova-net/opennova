# ---------------------------------------------------------------------------
# The public network load balancer: the gate and web (DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# One internet-facing network load balancer in both public subnets, cross-zone,
# each zone's node on an Elastic IP: zone a's is the gate's address
# (service.tf: prod's adopted one, 54.162.79.147, which retail's hosts file
# and the Switcher name, never released), zone b's one of its own. Retail
# clients reach the gate and web's HTTP routes at zone a's address alone
# (ONNET_PUBLIC_HOST, ONNET_WEB_HOST: no client follows a DNS name); the
# site's proxied records point at the balancer's DNS name (edge.tf), which
# answers with both. Listeners, to IP targets (the tasks) that see each
# client's own address:
#   gate            UDP gate_udp_port to the gate's tasks, from everyone (the
#                   clients' probes, the hosts' status blobs); health: HTTP
#                   /livez on health_port
#   https           TCP 443 to web's portal nginx (TLS end to end: Cloudflare's
#                   proxy to the origin certificate, edge.tf), from
#                   Cloudflare's published ranges alone; health: the handshake.
#                   With proxied records alone: without them there is no
#                   origin certificate, and nginx listens on 80
#   http            TCP game_http_port to web's server (the retail NW*.dll and
#                   .gsb routes; plain HTTP, stock clients), from everyone;
#                   health: /healthz (the stall watchdog, and the cache)
# The balancer's security group admits those ranges; the gate's and web's
# admit the balancer's group alone. ECS registers each task with its target
# groups and holds a deployment until the new tasks pass. Deletion protection
# is on: the balancer holds the gate's address (DEPLOY.md's rollback from the
# cutover turns it off first).

locals {
  lb_listeners = merge(
    {
      gate = { program = "gate", container = "gate", protocol = "UDP", port = var.gate_udp_port }
      http = { program = "web", container = "web", protocol = "TCP", port = var.game_http_port }
    },
    local.edge_proxied ? { https = { program = "web", container = "portal", protocol = "TCP", port = 443 } } : {},
  )

  # What the balancer sends each target group's tasks (their traffic and its
  # health checks): out of its group, into the program's.
  lb_target_rules = merge(
    {
      "gate"        = { program = "gate", protocol = "udp", port = var.gate_udp_port, description = "novaworld_gate UDP (the fixed port of stock clients), through the load balancer" }
      "gate-health" = { program = "gate", protocol = "tcp", port = var.health_port, description = "The gate /livez, the load balancer health check" }
      "http"        = { program = "web", protocol = "tcp", port = var.game_http_port, description = "HTTP routes of stock clients (NW*.dll, .gsb), through the load balancer" }
    },
    local.edge_proxied ? { "https" = { program = "web", protocol = "tcp", port = 443, description = "The portal nginx over TLS (Full strict), through the load balancer" } } : {},
  )

  # Who reaches each listener: everyone on the gate and the retail port,
  # Cloudflare's proxy alone on 443 (none without proxied records).
  lb_ingress = merge(
    { for c in var.allowed_cidrs : "gate ${c}" => { protocol = "udp", port = var.gate_udp_port, cidr = c, description = "novaworld_gate (the fixed UDP port of stock clients; the hosts status blobs)" } },
    { for c in var.allowed_cidrs : "http ${c}" => { protocol = "tcp", port = var.game_http_port, cidr = c, description = "HTTP routes of stock clients (NW*.dll, .gsb); /api/ answers nginx alone" } },
    { for c in(local.edge_proxied ? local.cloudflare_ipv4 : []) : "https ${c}" => { protocol = "tcp", port = 443, cidr = c, description = "The portal over TLS, from the Cloudflare proxy alone" } },
  )

  # Each zone's address: a the gate's, b its own.
  lb_allocations = {
    a = local.service_eip_allocation_id
    b = aws_eip.lb_b.allocation_id
  }
}

# Cloudflare's published ranges: the portal is reached through Cloudflare's
# proxy alone, so 443 admits nothing else (web/cloudflare-realip.conf trusts
# the same ranges for CF-Connecting-IP).
data "cloudflare_ip_ranges" "edge" {
  count = local.dns_enabled ? 1 : 0
}

locals {
  cloudflare_ipv4 = local.dns_enabled ? sort(data.cloudflare_ip_ranges.edge[0].ipv4_cidr_blocks) : []
}

# Zone b's address. Prod's carries prod's tag, so the deploy user cannot
# release it either (infra/aws/iam/opennova-deploy-network.json).
resource "aws_eip" "lb_b" {
  domain = "vpc"

  tags = merge(var.tags, {
    Name = "${local.name}-lb-b"
  })
}

resource "aws_security_group" "lb" {
  name        = "${local.name}-lb"
  description = "OpenNova NovaWorld load balancer: the gate UDP port and the retail HTTP port to everyone, 443 from Cloudflare alone"
  vpc_id      = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-lb"
  })
}

resource "aws_vpc_security_group_ingress_rule" "lb" {
  for_each = local.lb_ingress

  security_group_id = aws_security_group.lb.id
  description       = each.value.description
  ip_protocol       = each.value.protocol
  from_port         = each.value.port
  to_port           = each.value.port
  cidr_ipv4         = each.value.cidr
}

# Out: to the gate's and web's tasks alone, on their target ports and the
# gate's health port.
resource "aws_vpc_security_group_egress_rule" "lb" {
  for_each = local.lb_target_rules

  security_group_id            = aws_security_group.lb.id
  description                  = "To the ${each.value.program}: ${each.value.description}"
  ip_protocol                  = each.value.protocol
  from_port                    = each.value.port
  to_port                      = each.value.port
  referenced_security_group_id = aws_security_group.program[each.value.program].id
}

resource "aws_lb" "public" {
  name                             = local.name
  internal                         = false
  load_balancer_type               = "network"
  security_groups                  = [aws_security_group.lb.id]
  enable_cross_zone_load_balancing = true
  enable_deletion_protection       = true

  dynamic "subnet_mapping" {
    for_each = local.public_subnets
    content {
      subnet_id     = aws_subnet.public[subnet_mapping.key].id
      allocation_id = local.lb_allocations[subnet_mapping.key]
    }
  }

  tags = merge(var.tags, {
    Name = local.name
  })
}

resource "aws_lb_target_group" "public" {
  for_each = local.lb_listeners

  name        = "${local.name}-${each.key}"
  port        = each.value.port
  protocol    = each.value.protocol
  target_type = "ip"
  vpc_id      = aws_vpc.main.id
  # The retail routes and the rate limits read the TCP peer; UDP keeps it
  # always.
  preserve_client_ip   = "true"
  deregistration_delay = 30

  health_check {
    protocol            = each.key == "https" ? "TCP" : "HTTP"
    port                = each.key == "gate" ? tostring(var.health_port) : "traffic-port"
    path                = each.key == "gate" ? "/livez" : each.key == "http" ? "/healthz" : null
    matcher             = each.key == "https" ? null : "200"
    interval            = 10
    healthy_threshold   = 3
    unhealthy_threshold = 3
  }

  tags = merge(var.tags, {
    Name = "${local.name}-${each.key}"
  })
}

resource "aws_lb_listener" "public" {
  for_each = local.lb_listeners

  load_balancer_arn = aws_lb.public.arn
  port              = each.value.port
  protocol          = each.value.protocol

  default_action {
    type             = "forward"
    target_group_arn = aws_lb_target_group.public[each.key].arn
  }

  tags = var.tags
}
