# ---------------------------------------------------------------------------
# The public network load balancer: the gate, the lobby, web and the update
# service (DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# One internet-facing network load balancer in both public subnets, cross-zone,
# each zone's node on an Elastic IP: zone a's is the gate's address
# (service.tf: prod's adopted one, 54.162.79.147, which retail's hosts file
# and the Switcher name, never released), zone b's one of its own. Retail
# clients reach the gate, the lobby and web's HTTP routes at zone a's address
# alone (ONNET_PUBLIC_HOST, ONNET_LOBBY_ENDPOINT, ONNET_WEB_HOST), which the
# gate's own unproxied name, nw, also names (edge.tf; OpenNova's client dials
# it); the site's proxied records point at the balancer's DNS name (edge.tf),
# which answers with both, and so does the update service's unproxied one. Target groups, of IP targets (the tasks) that see each
# client's own address, each with its listener on its port:
#   gate            UDP gate_udp_port to the gate's tasks, from everyone (the
#                   clients' probes, the hosts' status blobs); health: HTTP
#                   /livez on health_port
#   lobby           UDP nw_udp_port to the lobby's tasks, from everyone: the
#                   one lobby address every lobby announces, so a stock host
#                   whose lobby task went away reconnects to it and lands on a
#                   live one (its session never asks the gate again). Each
#                   flow (a client's address and port) stays on one task, and a
#                   lobby sees each host's own address and port, which its
#                   listing's join target is; health: HTTP /livez on
#                   health_port
#   https           TCP 443 to web's portal nginx (TLS end to end: Cloudflare's
#                   proxy to the origin certificate, edge.tf), from
#                   Cloudflare's published ranges alone; health: the handshake.
#                   With proxied records alone: without them there is no
#                   origin certificate, and nginx listens on 80
#   http            TCP game_http_port to web's server (the retail NW*.dll and
#                   .gsb routes; plain HTTP, stock clients), from everyone;
#                   health: /healthz (the stall watchdog, and the cache)
#   update          TCP, the first of var.update_ports, to web's server: the
#                   update service retail UPDATE.EXE speaks (ADR 0061; web
#                   serves every update port, ONNET_UPDATE_PORTS), from
#                   everyone; health: the same /healthz on game_http_port.
#                   The other update ports get a listener each
#                   (aws_lb_listener.update), all forwarding to this one group
#                   (ECS admits five target groups per service; web holds
#                   three), so a task sees the updater on the first update
#                   port whichever it dialled
# The balancer's security group admits those ranges; the gate's and web's
# admit the balancer's group alone (the lobby's its UDP port from everyone
# too, programs.tf). ECS registers each task with its target groups and holds
# a deployment until the new tasks pass, and stops an old task only once its
# targets drained (deregistration_delay below). Deletion protection is on: the
# balancer holds the gate's address (DEPLOY.md's rollback from the cutover
# turns it off first).

locals {
  lb_listeners = merge(
    {
      gate   = { program = "gate", container = "gate", protocol = "UDP", port = var.gate_udp_port }
      lobby  = { program = "lobby", container = "lobby", protocol = "UDP", port = var.nw_udp_port }
      http   = { program = "web", container = "web", protocol = "TCP", port = var.game_http_port }
      update = { program = "web", container = "web", protocol = "TCP", port = var.update_ports[0] }
    },
    local.edge_proxied ? { https = { program = "web", container = "portal", protocol = "TCP", port = 443 } } : {},
  )

  # The update ports after the first: a listener each, forwarding to the
  # update group (its own listener is the one above, on the first port).
  lb_update_ports = { for p in slice(var.update_ports, 1, length(var.update_ports)) : tostring(p) => p }

  # What the balancer sends each target group's tasks (their traffic and its
  # health checks): out of its group, into the program's.
  lb_target_rules = merge(
    {
      "gate"         = { program = "gate", protocol = "udp", port = var.gate_udp_port, description = "novaworld_gate UDP (the fixed port of stock clients), through the load balancer" }
      "gate-health"  = { program = "gate", protocol = "tcp", port = var.health_port, description = "The gate /livez, the load balancer health check" }
      "lobby"        = { program = "lobby", protocol = "udp", port = var.nw_udp_port, description = "NovaWorld UDP, the lobby (the port the gate announces), through the load balancer" }
      "lobby-health" = { program = "lobby", protocol = "tcp", port = var.health_port, description = "The lobby /livez, the load balancer health check" }
      "http"         = { program = "web", protocol = "tcp", port = var.game_http_port, description = "HTTP routes of stock clients (NW*.dll, .gsb), through the load balancer" }
      "update"       = { program = "web", protocol = "tcp", port = var.update_ports[0], description = "The update service of retail UPDATE.EXE (every update port lands here), through the load balancer" }
    },
    local.edge_proxied ? { "https" = { program = "web", protocol = "tcp", port = 443, description = "The portal nginx over TLS (Full strict), through the load balancer" } } : {},
  )

  # Who reaches each listener: everyone on the gate, the lobby, the retail
  # port and the update ports, Cloudflare's proxy alone on 443 (none without
  # proxied records).
  lb_ingress = merge(
    { for c in var.allowed_cidrs : "gate ${c}" => { protocol = "udp", port = var.gate_udp_port, cidr = c, description = "novaworld_gate (the fixed UDP port of stock clients; the hosts status blobs)" } },
    { for c in var.allowed_cidrs : "lobby ${c}" => { protocol = "udp", port = var.nw_udp_port, cidr = c, description = "NovaWorld UDP, the lobby (the port the gate announces)" } },
    { for c in var.allowed_cidrs : "http ${c}" => { protocol = "tcp", port = var.game_http_port, cidr = c, description = "HTTP routes of stock clients (NW*.dll, .gsb); /api/ answers nginx alone" } },
    merge([for p in var.update_ports : { for c in var.allowed_cidrs : "update_${p} ${c}" => { protocol = "tcp", port = p, cidr = c, description = "The update service of retail UPDATE.EXE (one of its fixed TCP ports)" } }]...),
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

# Its description predates the lobby's listener and the update ports and
# stays: a changed one replaces the group, which the balancer holds.
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

# Out: to the gate's, the lobby's and web's tasks alone, on their target ports
# (the update group's the first update port) and the gate's and the lobby's
# health port.
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
  # always (the lobby lists a host at the address and port it sees).
  preserve_client_ip = "true"
  # How long a deregistering task keeps the flows it has, which ECS waits out
  # before it stops the task: 30 s for a gate's reply or a request in flight;
  # 10 s for the lobby, long enough for an exchange in flight (a join's 3 s
  # resends, a command's 250 ms take) and short because no session can follow
  # its task anyway: a host whose flow moves reaches a lobby that holds no
  # session for it, and lists again by timing out (240 s) and reconnecting,
  # so a longer delay would only hold the deploy.
  deregistration_delay = each.key == "lobby" ? 10 : 30
  # A UDP flow ends with the delay. Left open, a flow its client's keepalives
  # keep fresh would stay on the deregistered task past its stop, and the
  # client's reconnect, from the same address and port, would ride it to a
  # target that is gone.
  connection_termination = each.value.protocol == "UDP" ? true : null

  # The update group speaks the updater's protocol on its port, so its check
  # goes to the retail port's /healthz.
  health_check {
    protocol            = each.key == "https" ? "TCP" : "HTTP"
    port                = each.value.protocol == "UDP" ? tostring(var.health_port) : each.key == "update" ? tostring(var.game_http_port) : "traffic-port"
    path                = each.value.protocol == "UDP" ? "/livez" : each.key == "http" || each.key == "update" ? "/healthz" : null
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

# The other update ports, each to the one update group.
resource "aws_lb_listener" "update" {
  for_each = local.lb_update_ports

  load_balancer_arn = aws_lb.public.arn
  port              = each.value
  protocol          = "TCP"

  default_action {
    type             = "forward"
    target_group_arn = aws_lb_target_group.public["update"].arn
  }

  tags = var.tags
}
