output "managed_repositories" {
  value       = [for r in github_repository.managed : r.full_name]
  description = "Full names (owner/repo) of the managed expansion repositories"
}

output "managed_secret_count" {
  value       = length(github_actions_secret.repository)
  description = "Number of Actions secrets managed across the expansion repos"
}
