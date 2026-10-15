# ---------------------------------------------------------------------------
# What drives the service host: SSM (DEPLOY.md, section 4)
# ---------------------------------------------------------------------------
#
# Under /opennova/<deployment>/:
#   infra.env   (String, here) what Terraform knows that the stack needs: the
#               public address, the site's URL, the database's URL (no
#               password), the hosted machines' settings, the operations bucket
#               and log group; one KEY='value' per line, an env file for docker
#               compose
#   release     (String) the id of the release the host runs; `app deploy` and
#               `app rollback` write it (Terraform makes it, then leaves its
#               value alone)
#   secret/*    (SecureString) the runtime secrets, which `./deploy/run.sh secrets
#               sync` copies from the vault (deploy/env/runtime.env.tpl);
#               Terraform never sees them
# The converge document holds deploy/host/converge.sh: `app deploy` runs it on
# the host through Run Command, and the host runs it at every boot. It reads
# the parameters above into memory-only env files and brings the containers to
# the release.

locals {
  ssm_prefix = "/opennova/${local.deployment}"

  # The backup sidecar's key prefix, one path segment (it names its dumps after
  # it: <prefix>/<prefix>-<time>.dump): prod keeps the sidecar's default.
  backup_prefix = local.manage_shared ? "novaworld" : "novaworld-${local.deployment}"

  infra_env = merge(
    {
      OPENNOVA_DEPLOYMENT         = local.deployment
      OPENNOVA_CONVERGE_DOCUMENT  = aws_ssm_document.converge.name
      OPENNOVA_OPS_BUCKET         = var.ops_bucket_name
      OPENNOVA_LOG_GROUP          = aws_cloudwatch_log_group.service.name
      AWS_REGION                  = var.region
      AWS_DEFAULT_REGION          = var.region
      BACKUP_BUCKET               = var.ops_bucket_name
      BACKUP_PREFIX               = local.backup_prefix
      ONNET_PUBLIC_HOST           = local.service_public_ip
      OPENNOVA_SERVICE_PRIVATE_IP = local.service_private_ip
      ONNET_SITE_URL              = local.dns_enabled ? "https://${local.web_host}" : ""
    },
    local.database_env,
    local.hosted_env,
  )

  # One KEY='value' per line, sorted: an env file docker compose reads
  # literally (single quotes: no interpolation), and the host's scripts parse.
  infra_env_text = join("", [for k in sort(keys(local.infra_env)) : "${k}='${local.infra_env[k]}'\n"])
}

resource "aws_ssm_document" "converge" {
  name            = "${local.name}-converge"
  document_type   = "Command"
  document_format = "JSON"
  target_type     = "/AWS::EC2::Instance"

  content = jsonencode({
    schemaVersion = "2.2"
    description   = "OpenNova: converge the ${local.deployment} service host to its current release (deploy/host/converge.sh)"
    mainSteps = [{
      action = "aws:runShellScript"
      name   = "converge"
      inputs = {
        timeoutSeconds = "1200"
        runCommand     = [file("${path.module}/../../deploy/host/converge.sh")]
      }
    }]
  })

  tags = var.tags
}

resource "aws_ssm_parameter" "infra_env" {
  name        = "${local.ssm_prefix}/infra.env"
  description = "What Terraform knows that the ${local.deployment} stack needs (an env file; infra/aws/service_ssm.tf)"
  type        = "String"
  tier        = "Standard"
  value       = local.infra_env_text

  lifecycle {
    precondition {
      condition     = alltrue([for v in values(local.infra_env) : !strcontains(v, "'") && !strcontains(v, "\n")])
      error_message = "an infra.env value holds a single quote or a newline, which its env-file form cannot carry"
    }
  }

  tags = var.tags
}

resource "aws_ssm_parameter" "release" {
  name        = "${local.ssm_prefix}/release"
  description = "The release the ${local.deployment} service host runs (./deploy/run.sh app deploy | app rollback)"
  type        = "String"
  tier        = "Standard"
  value       = "none"

  lifecycle {
    ignore_changes = [value]
  }

  tags = var.tags
}
