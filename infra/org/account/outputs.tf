output "state_bucket" {
  value       = aws_s3_bucket.state.bucket
  description = "The account's Terraform state bucket"
}

output "deploy_user_arn" {
  value       = aws_iam_user.deploy.arn
  description = "The deploy user the toolbox acts as in this account"
}

output "access_key_id" {
  value       = aws_iam_access_key.deploy.id
  description = "The deploy user's key id (the vault's aws-<deployment>/access_key_id)"
}

output "secret_access_key" {
  value       = aws_iam_access_key.deploy.secret
  sensitive   = true
  description = "The deploy user's secret key (the vault's aws-<deployment>/secret_access_key; never printed)"
}

output "github_deploy_role_arn" {
  value       = aws_iam_role.github_deploy.arn
  description = "The role the deploy workflow's environment of the deployment's name assumes"
}

# For the tests: what each policy says.
output "documents" {
  value = {
    boundary         = aws_iam_policy.boundary.policy
    machine_boundary = aws_iam_policy.machine_boundary.policy
    deploy           = { for k, p in aws_iam_policy.deploy : k => p.policy }
    state            = aws_iam_user_policy.state.policy
    guard            = aws_iam_user_policy.guard.policy
    bucket           = aws_s3_bucket_policy.state.policy
    github_trust     = aws_iam_role.github_deploy.assume_role_policy
    github           = aws_iam_role_policy.github_deploy.policy
    github_boundary  = aws_iam_role.github_deploy.permissions_boundary
  }
  description = "Each policy document the account's baseline holds"
}
