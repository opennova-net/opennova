# ---------------------------------------------------------------------------
# The service database: PostgreSQL 17 on RDS (DEPLOY.md, section 3)
# ---------------------------------------------------------------------------
#
# One single-AZ instance in the private subnets, beside the service instance (public
# subnet a's zone), reached from the service instance's security group alone. TLS
# is forced (rds.force_ssl); the server and the host's tools verify it against
# the RDS CA bundle (sslmode=verify-full; the server image pins the bundle,
# /app/rds-global-bundle.pem). Encrypted gp3 storage that grows on its own up to
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

resource "aws_vpc_security_group_ingress_rule" "db_from_service" {
  security_group_id            = aws_security_group.db.id
  description                  = "PostgreSQL from the service instance"
  ip_protocol                  = "tcp"
  from_port                    = 5432
  to_port                      = 5432
  referenced_security_group_id = aws_security_group.service.id
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

  # Statements slower than a second reach the instance's postgresql log.
  parameter {
    name  = "log_min_duration_statement"
    value = "1000"
  }

  tags = var.tags

  lifecycle {
    create_before_destroy = true
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
  availability_zone      = aws_subnet.private["a"].availability_zone
  multi_az               = false
  publicly_accessible    = false
  port                   = 5432
  ca_cert_identifier     = "rds-ca-rsa2048-g1"

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
  # The server's connection: the service's own role and database over verified
  # TLS. The password is DATABASE_PASSWORD, a secret (secrets sync), never here.
  database_url = "postgresql://${var.db_user}@${aws_db_instance.main.address}:${aws_db_instance.main.port}/${var.db_name}?sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem"

  database_env = {
    DATABASE_URL            = local.database_url
    OPENNOVA_DB_HOST        = aws_db_instance.main.address
    OPENNOVA_DB_PORT        = tostring(aws_db_instance.main.port)
    OPENNOVA_DB_NAME        = var.db_name
    OPENNOVA_DB_USER        = var.db_user
    OPENNOVA_DB_MASTER_USER = var.db_master_username
    OPENNOVA_DB_IDENTIFIER  = aws_db_instance.main.identifier
  }
}
