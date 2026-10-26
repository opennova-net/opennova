# The deploy workflow's environments (.github/workflows/deploy.yml; DEPLOY.md,
# "The deploy workflow"): one per deployment, each holding the role its jobs
# assume in that deployment's own AWS account (infra/org/account/github.tf
# makes it, trusting repo:<owner>/<repository>:environment:<deployment> alone). prod's runs wait
# for an organization admin's approval and come from protected branches alone;
# staging's run at once, from any branch, to rehearse. The accounts and
# regions are the vault's (items aws-prod and aws-staging, through
# deploy/env/github.env.tpl): no tracked file names one.

locals {
  deploy_accounts = {
    prod    = { account_id = var.aws_account_id_prod, region = var.aws_region_prod, site = "https://${var.domain}", admin_api_token = var.admin_api_token_prod }
    staging = { account_id = var.aws_account_id_staging, region = var.aws_region_staging, site = "https://staging.${var.domain}", admin_api_token = var.admin_api_token_staging }
  }

  # Who approves a prod deploy: the organization's admins.
  deploy_approvers = sort([for login, role in var.members : login if role == "admin"])

  deploy_environment_variables = merge([
    for d, a in local.deploy_accounts : {
      "${d}/AWS_DEPLOY_ROLE_ARN" = { environment = d, name = "AWS_DEPLOY_ROLE_ARN", value = "arn:aws:iam::${a.account_id}:role/opennova-${d}-github-deploy" }
      "${d}/AWS_REGION"          = { environment = d, name = "AWS_REGION", value = a.region }
      "${d}/AWS_ACCOUNT_ID"      = { environment = d, name = "AWS_ACCOUNT_ID", value = a.account_id }
      "${d}/NOVAWORLD_URL"       = { environment = d, name = "NOVAWORLD_URL", value = a.site }
    }
  ]...)
}

data "github_user" "deploy_approver" {
  for_each = toset(local.deploy_approvers)
  username = each.key
}

resource "github_repository_environment" "prod" {
  repository          = var.repository
  environment         = "prod"
  can_admins_bypass   = false
  prevent_self_review = false

  reviewers {
    users = [for login in local.deploy_approvers : tonumber(data.github_user.deploy_approver[login].id)]
  }

  deployment_branch_policy {
    protected_branches     = true
    custom_branch_policies = false
  }

  depends_on = [github_repository.repo]
}

resource "github_repository_environment" "staging" {
  repository  = var.repository
  environment = "staging"

  depends_on = [github_repository.repo]
}

# The Publish workflow's (.github/workflows/publish.yml) admin API token, with which it publishes
# a build's runtime release to the deployment's NOVAWORLD_URL: the deployment's own
# (app-<deployment>/admin_api_token, the vault's; DEPLOY.md, "The downloads host").
resource "github_actions_environment_secret" "admin_api_token" {
  for_each    = toset(keys(local.deploy_accounts))
  repository  = var.repository
  environment = each.key == "prod" ? github_repository_environment.prod.environment : github_repository_environment.staging.environment
  secret_name = "ADMIN_API_TOKEN"
  value       = local.deploy_accounts[each.key].admin_api_token
}

resource "github_actions_environment_variable" "deploy" {
  for_each      = local.deploy_environment_variables
  repository    = var.repository
  environment   = each.value.environment == "prod" ? github_repository_environment.prod.environment : github_repository_environment.staging.environment
  variable_name = each.value.name
  value         = each.value.value
}
