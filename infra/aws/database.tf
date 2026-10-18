# ---------------------------------------------------------------------------
# The service database: PostgreSQL 17 on RDS (DEPLOY.md, section 4, "The database")
# ---------------------------------------------------------------------------
#
# One instance in the private subnets on db.t4g.micro (burstable, which RDS runs
# in unlimited mode: billed past its baseline, never throttled), single-AZ in
# a zone RDS picks while traffic is small (var.db_multi_az on: RDS keeps a
# synchronous standby in the other private subnet's zone and fails over to it on
# its own, the endpoint's name following). Reached from the five programs'
# security groups and the backup task's alone (programs.tf, backup.tf).
# max_connections is sized, not left to RDS's memory formula: db_pool_max
# connections for each of db_service_processes program processes, plus 10 for
# the backup task, the operator's psql and the reserved superuser and RDS
# connections. TLS is forced (rds.force_ssl); every client verifies it against
# the RDS CA bundle (sslmode=verify-full; every program's image and the backup
# image pin the bundle at /app/rds-global-bundle.pem). Encrypted gp3 storage that grows on its own up to
# var.db_max_allocated_storage_gb, 14 days of point-in-time recovery, a final
# snapshot and deletion protection; Terraform refuses to plan its destruction
# (prevent_destroy), and the toolbox's `infra apply` refuses a plan that would.
#
# The master user exists for `./deploy/run.sh db bootstrap` alone, which makes
# the service's own role and its database (C collation, owned by that role).
# Its password is write-only (password_wo): it comes from the vault
# (TF_VAR_db_master_password, an ephemeral variable) and never enters the state
# or a plan; bump var.db_master_password_version to rotate it. The service's
# password (DATABASE_PASSWORD) never reaches Terraform at all.

# Its description keeps the wording it was made with: AWS fixes a group's
# description for the group's life, so a changed one replaces the group.
resource "aws_security_group" "db" {
  name        = "${local.name}-db"
  description = "OpenNova NovaWorld database: PostgreSQL from the service host alone"
  vpc_id      = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-db"
  })
}

locals {
  # Who reaches the database: every program and the backup task.
  database_clients = merge(
    { for p in keys(local.programs) : p => aws_security_group.program[p].id },
    { backup = aws_security_group.backup.id },
  )
}

resource "aws_vpc_security_group_ingress_rule" "db_from_client" {
  for_each = local.database_clients

  security_group_id            = aws_security_group.db.id
  description                  = "PostgreSQL from the ${each.key}"
  ip_protocol                  = "tcp"
  from_port                    = 5432
  to_port                      = 5432
  referenced_security_group_id = each.value
}

resource "aws_db_subnet_group" "main" {
  name        = "${local.name}-db"
  description = "OpenNova NovaWorld database: the private subnets"
  subnet_ids  = [for k in sort(keys(aws_subnet.private)) : aws_subnet.private[k].id]

  tags = var.tags
}

resource "aws_db_parameter_group" "main" {
  name        = "${local.name}-postgres17"
  family      = "postgres17"
  description = "OpenNova NovaWorld database: TLS required, slow statements logged"

  # RDS records it as pending-reboot; left at Terraform's default (immediate),
  # every plan would update it again.
  parameter {
    name         = "rds.force_ssl"
    value        = "1"
    apply_method = "pending-reboot"
  }

  # Static: it takes effect at the instance's next reboot (a class change or a
  # failover is one).
  parameter {
    name         = "max_connections"
    value        = tostring(local.db_max_connections)
    apply_method = "pending-reboot"
  }

  # Statements slower than a second reach the instance's postgresql log.
  parameter {
    name  = "log_min_duration_statement"
    value = "1000"
  }

  tags = var.tags

  lifecycle {
    create_before_destroy = true

    # max_connections holds every program's tasks at its max, with a
    # deployment's extra tasks (programs.tf).
    precondition {
      condition     = local.program_processes_max <= var.db_service_processes
      error_message = "the programs may run ${local.program_processes_max} processes at once (each at its max, and a deployment's extra tasks beside them), more than db_service_processes (${var.db_service_processes}) sizes max_connections for: raise db_service_processes or lower a program's max."
    }
  }
}

resource "aws_db_instance" "main" {
  identifier     = "${local.name}-db"
  engine         = "postgres"
  engine_version = "17"
  instance_class = var.db_instance_class

  username            = var.db_master_username
  password_wo         = var.db_master_password
  password_wo_version = var.db_master_password_version

  allocated_storage     = var.db_allocated_storage_gb
  max_allocated_storage = var.db_max_allocated_storage_gb
  storage_type          = "gp3"
  storage_encrypted     = true

  db_subnet_group_name   = aws_db_subnet_group.main.name
  vpc_security_group_ids = [aws_security_group.db.id]
  parameter_group_name   = aws_db_parameter_group.main.name
  # No zone: RDS places the instance in the subnet group itself. Turning
  # Multi-AZ off keeps whichever zone the primary is in, so a pinned zone
  # would plan a replacement (prod, 2026-10-17: us-east-1b).
  multi_az            = var.db_multi_az
  publicly_accessible = false
  port                = 5432
  ca_cert_identifier  = "rds-ca-rsa2048-g1"

  backup_retention_period   = var.db_backup_retention_days
  backup_window             = "08:00-08:30"
  maintenance_window        = "sun:09:00-sun:09:30"
  delete_automated_backups  = false
  copy_tags_to_snapshot     = true
  deletion_protection       = true
  skip_final_snapshot       = false
  final_snapshot_identifier = "${local.name}-db-final"

  auto_minor_version_upgrade  = true
  allow_major_version_upgrade = false
  apply_immediately           = false

  performance_insights_enabled          = true
  performance_insights_retention_period = 7

  tags = merge(var.tags, {
    Name = "${local.name}-db"
  })

  lifecycle {
    prevent_destroy = true
  }
}

locals {
  # Every program process's pool, plus the backup task, the operator's psql
  # and the reserved superuser and RDS connections.
  db_max_connections = var.db_pool_max * var.db_service_processes + 10

  # The programs' connection: the service's own role and database over
  # verified TLS. The password is DATABASE_PASSWORD, a secret (secrets sync),
  # never here.
  database_url = "postgresql://${var.db_user}@${aws_db_instance.main.address}:${aws_db_instance.main.port}/${var.db_name}?sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem"

  # Every program's (and the backup task's) environment.
  database_env = {
    DATABASE_URL      = local.database_url
    DATABASE_POOL_MAX = tostring(var.db_pool_max)
  }

  # The toolbox's (infra.env): the db commands' names.
  database_toolbox_env = {
    OPENNOVA_DB_HOST        = aws_db_instance.main.address
    OPENNOVA_DB_PORT        = tostring(aws_db_instance.main.port)
    OPENNOVA_DB_NAME        = var.db_name
    OPENNOVA_DB_USER        = var.db_user
    OPENNOVA_DB_MASTER_USER = var.db_master_username
    OPENNOVA_DB_IDENTIFIER  = aws_db_instance.main.identifier
  }
}
