# The repository's Actions secrets whose values live in the vault. Terraform
# keeps a secret's value in its state (the vault document tfstate-github-org), as
# infra/aws keeps its own; GitHub never hands a secret back, so a value set by
# hand in the repository settings is overwritten at the next apply.

# REFERENCE_ASSETS_TOKEN: read access (Contents: Read-only) to the two private
# reference repositories the retail legs mount
# (.github/actions/mount-reference-data; docs/asset-gated-tests.md). Its value is
# the vault item reference-assets.
resource "github_actions_secret" "reference_assets_token" {
  repository  = var.repository
  secret_name = "REFERENCE_ASSETS_TOKEN"
  value       = var.reference_assets_token
}
