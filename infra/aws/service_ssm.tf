# ---------------------------------------------------------------------------
# What the deploy toolbox reads, and the deployment's release: SSM (DEPLOY.md,
# section 4)
# ---------------------------------------------------------------------------
#
# Under /opennova/<deployment>/:
#   infra.env   (String, here) what Terraform knows that the toolbox needs (it
#               runs no Terraform for a deploy, and neither does the deploy
#               workflow): the cluster, each program's task definition (the
#               template a deploy clones) and the backup task's, the subnets
#               and security group of the toolbox's one-off tasks, the image
#               registry, the database's names, the hosted machines' settings,
#               the account, the operations bucket and the log groups; one
#               KEY='value' per line
#   release     (String) the deployment's release: its manifest (the images by
#               digest, the task definition revision each program runs), which
#               `app deploy`, `app converge` and `app rollback` write
#               (Terraform makes it, then leaves its value alone)
#   secret/*    (SecureString) the runtime secrets, which `./deploy/run.sh secrets
#               sync` copies from the vault (deploy/env/runtime.env.tpl) and ECS
#               hands each task at its start; Terraform never sees them
#   tls/*       (SecureString) the origin certificate and its key (edge.tf)
# The tasks read nothing here but their secrets: their settings are their task
# definitions' environment (programs.tf).

locals {
  ssm_prefix = "/opennova/${local.deployment}"

  # The backup task's key prefix in the deployment's own bucket, one path
  # segment (it names its dumps after it: <prefix>/<prefix>-<time>.dump).
  backup_prefix = "novaworld"

  infra_env = merge(
    {
      OPENNOVA_DEPLOYMENT         = local.deployment
      OPENNOVA_ACCOUNT_ID         = var.aws_account_id
      OPENNOVA_OPS_BUCKET         = local.ops_bucket_name
      OPENNOVA_LOG_GROUP          = aws_cloudwatch_log_group.service.name
      OPENNOVA_MACHINES_LOG_GROUP = aws_cloudwatch_log_group.machines.name
      AWS_REGION                  = local.region
      BACKUP_BUCKET               = local.ops_bucket_name
      BACKUP_PREFIX               = local.backup_prefix
      # The cluster and its services (the programs' names), each program's
      # task definition as Terraform made it and the backup task's, as JSON.
      OPENNOVA_CLUSTER          = aws_ecs_cluster.main.name
      OPENNOVA_TASK_DEFINITIONS = jsonencode(merge({ for p, d in aws_ecs_task_definition.program : p => d.arn }, { backup = aws_ecs_task_definition.backup.arn }))
      # Each program's task counts: the first deploy starts each service at
      # its desired count and raises its floor to its min (Terraform's next
      # apply sets the same).
      OPENNOVA_PROGRAM_COUNTS = jsonencode({ for p, c in local.programs : p => { min = c.min, desired = c.desired, max = c.max } })
      # Where the toolbox's one-off tasks (db, backup) run.
      OPENNOVA_TASK_SUBNETS          = join(",", local.program_subnet_ids)
      OPENNOVA_BACKUP_SECURITY_GROUP = aws_security_group.backup.id
      # The nightly backup's schedule, which a release points at its backup
      # revision (backup.tf).
      OPENNOVA_BACKUP_SCHEDULE = aws_scheduler_schedule.backup.name
      # The registry the serve and hostd images are resolved in.
      OPENNOVA_IMAGE_REGISTRY = var.image_registry
      # The gate's address, the one retail clients reach.
      ONNET_PUBLIC_HOST = local.service_public_ip
    },
    local.database_toolbox_env,
    local.fleet_toolbox_env,
  )

  # One KEY='value' per line, sorted: the toolbox parses it.
  infra_env_text = join("", [for k in sort(keys(local.infra_env)) : "${k}='${local.infra_env[k]}'\n"])
}

resource "aws_ssm_parameter" "infra_env" {
  name        = "${local.ssm_prefix}/infra.env"
  description = "What Terraform knows that the ${local.deployment} deploy toolbox needs (KEY='value' lines; infra/aws/service_ssm.tf)"
  type        = "String"
  tier        = "Standard"
  value       = local.infra_env_text

  lifecycle {
    precondition {
      condition     = alltrue([for v in values(local.infra_env) : !strcontains(v, "'") && !strcontains(v, "\n")])
      error_message = "an infra.env value holds a single quote or a newline, which its KEY='value' form cannot carry"
    }
  }

  tags = var.tags
}

# The release as it stands (the toolbox's writes, which the resource ignores):
# whether the programs have one decides whether their services run tasks
# (programs.tf, local.released). Read by path, so a stack's first plan, before
# the parameter exists, reads none.
data "aws_ssm_parameters_by_path" "deployment" {
  path = local.ssm_prefix
}

locals {
  release_index = index(concat(data.aws_ssm_parameters_by_path.deployment.names, ["${local.ssm_prefix}/release"]), "${local.ssm_prefix}/release")
  release_value = try(nonsensitive(data.aws_ssm_parameters_by_path.deployment.values[local.release_index]), "none")
}

resource "aws_ssm_parameter" "release" {
  name        = "${local.ssm_prefix}/release"
  description = "The release the ${local.deployment} programs run, its manifest (./deploy/run.sh app deploy | app converge | app rollback)"
  type        = "String"
  tier        = "Standard"
  value       = "none"

  lifecycle {
    ignore_changes = [value]
  }

  tags = var.tags
}
