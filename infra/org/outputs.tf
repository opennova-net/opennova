# What the toolbox writes back to the vault after an apply: per deployment,
# aws-<deployment>/state_bucket, github_deploy_role_arn, and the deploy user's
# access_key_id and secret_access_key (from deploy_keys, straight into the
# vault, never printed), which every later run of that deployment reads.

locals {
  member_modules = {
    prod    = module.prod
    staging = module.staging
  }
}

output "deployments" {
  value = {
    for d, m in local.member_modules : d => {
      account_id             = aws_organizations_account.member[d].id
      state_bucket           = m.state_bucket
      deploy_user_arn        = m.deploy_user_arn
      github_deploy_role_arn = m.github_deploy_role_arn
    }
  }
  description = "Each deployment's account, its state bucket, its deploy user and the role its deploy workflow assumes"
}

output "deploy_keys" {
  value = {
    for d, m in local.member_modules : d => {
      access_key_id     = m.access_key_id
      secret_access_key = m.secret_access_key
    }
  }
  sensitive   = true
  description = "Each deployment's deploy user key, for the vault item aws-<deployment> alone"
}

# opennova-org's own policy (infra/aws/iam/opennova-org.json) filled for this
# organization, for the management account's administrator to apply by hand
# (opennova-org cannot change its own permissions): after a new account, an
# apply prints the policy that names it too. opennova-org also runs infra/ci
# as the ci account's OrganizationAccountAccessRole.
output "org_user_policy" {
  value = jsonencode(jsondecode(templatefile("${path.module}/../aws/iam/opennova-org.json", {
    management_account_id = var.management_account_id
    organization_id       = var.organization_id
    prod_account_id       = var.aws_account_id_prod
    staging_account_id    = var.aws_account_id_staging
    ci_account_id         = var.aws_account_id_ci
  })))
  description = "opennova-org's IAM policy as the administrator applies it"
}

# Identity Center's portal, where the maintainer signs in to the member
# accounts (the instance's default address; one customized in the console is
# that one instead): the vault's aws-org/sso_portal_url.
output "sso_portal_url" {
  value       = "https://${local.identity_store_id}.awsapps.com/start"
  description = "The IAM Identity Center portal's default address"
}
