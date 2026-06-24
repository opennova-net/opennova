variable "github_token" {
  description = "GitHub token with repo + admin:repo_hook scopes on the owner org"
  type        = string
  sensitive   = true
}

variable "github_owner" {
  description = "GitHub organization or user that owns the expansion repositories"
  type        = string
  default     = "opennova-net"
}

variable "shared_repository_secrets" {
  description = <<-EOT
    Actions secrets applied to EVERY expansion repo (local.expansions). The same
    values go on every repo, so they're declared once here rather than per-repo.
    The build workflow expects:
      AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY  (S3 upload to the downloads bucket;
        the launcher_ci keys output by infra/aws are reused)
      EXPANSION_PUBLISH_TOKEN                    (bearer for the server publish callback;
        same value as app-prod/expansion_publish_token)
      NOVAWORLD_SERVER_URL                       (publish callback base, e.g. https://nw.<domain>)
      DOWNLOADS_BUCKET                           (S3 bucket name for the upload)
  EOT
  type      = map(string)
  default   = {}
  sensitive = true
}
