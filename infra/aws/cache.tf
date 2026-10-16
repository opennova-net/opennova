# ---------------------------------------------------------------------------
# Web's ephemeral state: ElastiCache for Valkey (DEPLOY.md, section 3, "The
# cache")
# ---------------------------------------------------------------------------
#
# What web's tasks share and may lose: the retail login, join and host-key
# sessions, the pin cache and the rate limiters, each with its TTL.
# Everything with an invariant stays in PostgreSQL (database.tf). A replication
# group of two nodes, the primary in one private subnet's zone and a replica in
# the other's, failing over on its own (the primary endpoint's name follows
# it). In-transit TLS is required (rediss://; ElastiCache's certificates chain
# to Amazon's public roots, which web's image's system bundle trusts) and
# every client authenticates with the AUTH token, the vault's
# app-<deployment>/redis_password: Terraform hands it to ElastiCache
# (TF_VAR_cache_auth_token, deploy/env/terraform.env.tpl; the state holds it,
# as it holds the edge authentication secret), and `secrets sync` hands it to
# web as ONNET_REDIS_PASSWORD (deploy/env/runtime.env.tpl). At-rest
# encryption on, no snapshots (nothing here outlives a TTL), maxmemory-policy
# volatile-lru (ElastiCache's default, named here): every key web writes has a
# TTL, so a full node evicts the key touched longest ago rather than refuse a
# login (the evictions alarm, observability.tf, says the node is too small).
# Cluster mode stays off: one script writes a pin and its account's index
# together, which a single shard alone allows. Reached from web alone: its
# security group admits port 6379 from web's.
#
# Web's environment names it: ONNET_REDIS_URL (rediss://<primary endpoint>:6379).

locals {
  cache_port = 6379

  # Two nodes, the primary and its replica: ElastiCache names them
  # <group>-001 and <group>-002 (the alarms watch each).
  cache_group_id = "${local.name}-cache"
  cache_nodes    = [for i in range(1, 3) : format("%s-%03d", local.cache_group_id, i)]

  cache_env = {
    ONNET_REDIS_URL = "rediss://${aws_elasticache_replication_group.cache.primary_endpoint_address}:${local.cache_port}"
  }
}

# Its description keeps the wording it was made with: AWS fixes a group's
# description for the group's life, so a changed one replaces the group.
resource "aws_security_group" "cache" {
  name        = "${local.name}-cache"
  description = "OpenNova NovaWorld cache: Valkey over TLS from web alone"
  vpc_id      = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-cache"
  })
}

resource "aws_vpc_security_group_ingress_rule" "cache_from_web" {
  security_group_id            = aws_security_group.cache.id
  description                  = "Valkey (TLS) from web"
  ip_protocol                  = "tcp"
  from_port                    = local.cache_port
  to_port                      = local.cache_port
  referenced_security_group_id = aws_security_group.program["web"].id
}

resource "aws_elasticache_subnet_group" "cache" {
  name        = "${local.name}-cache"
  description = "OpenNova NovaWorld cache: the private subnets"
  subnet_ids  = [for k in sort(keys(aws_subnet.private)) : aws_subnet.private[k].id]

  tags = var.tags
}

resource "aws_elasticache_parameter_group" "cache" {
  name        = "${local.name}-valkey8"
  family      = "valkey8"
  description = "OpenNova NovaWorld cache: evict the least recently used key"

  # Every key has a TTL: a full node evicts the one touched longest ago, as
  # the in-memory limiters' full tables did, rather than fail every write.
  parameter {
    name  = "maxmemory-policy"
    value = "volatile-lru"
  }

  tags = var.tags

  lifecycle {
    create_before_destroy = true
  }
}

resource "aws_elasticache_replication_group" "cache" {
  replication_group_id = local.cache_group_id
  description          = "OpenNova NovaWorld web state (sessions, pins, rate limits)"
  engine               = "valkey"
  engine_version       = "8.0"
  node_type            = var.cache_node_type
  port                 = local.cache_port
  parameter_group_name = aws_elasticache_parameter_group.cache.name

  num_cache_clusters          = 2
  preferred_cache_cluster_azs = local.azs
  automatic_failover_enabled  = true
  multi_az_enabled            = true

  subnet_group_name  = aws_elasticache_subnet_group.cache.name
  security_group_ids = [aws_security_group.cache.id]

  at_rest_encryption_enabled = true
  transit_encryption_enabled = true
  transit_encryption_mode    = "required"
  auth_token                 = var.cache_auth_token
  # A changed token joins the old one (both valid) until the service has the
  # new one (DEPLOY.md, "The cache": rotating).
  auth_token_update_strategy = "ROTATE"

  snapshot_retention_limit   = 0
  maintenance_window         = "sun:09:30-sun:10:30"
  auto_minor_version_upgrade = true
  # ElastiCache takes a changed AUTH token only with apply_immediately.
  apply_immediately = true

  tags = merge(var.tags, {
    Name = local.cache_group_id
  })
}
