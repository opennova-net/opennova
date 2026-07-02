# GitHub side of the OpenNova NovaWorld deployment: the expansion content
# repositories and their GitHub Actions secrets. Ported from opennova-int's
# infra/github so the whole stack — AWS (infra/aws) AND the GitHub repos that
# feed the expansion-publish pipeline — boots from our own infra.
#
# The expansion repos already exist, so they are IMPORTED once before the
# first apply (see README.md / DEPLOY.md). prevent_destroy + archive_on_destroy
# make adoption safe; on a fresh org `terraform apply` creates them instead.

terraform {
  required_version = ">= 1.6.0"

  required_providers {
    github = {
      source  = "integrations/github"
      version = "~> 5.40"
    }
    local = {
      source  = "hashicorp/local"
      version = "~> 2.4"
    }
  }
}

provider "github" {
  token = var.github_token
  owner = var.github_owner
}

# One repo per expansion (local.expansions, expansions.tf). Their build
# workflows tag -> package -> upload to S3 -> call the server's
# /admin/internal/.../publish callback.
resource "github_repository" "managed" {
  for_each = local.expansions

  name                   = each.key
  description            = each.value.description
  visibility             = "private"
  has_issues             = true
  has_projects           = false
  has_wiki               = false
  allow_merge_commit     = true
  allow_rebase_merge     = true
  allow_squash_merge     = true
  delete_branch_on_merge = true
  vulnerability_alerts   = true
  archive_on_destroy     = true
  # A fresh repo needs a default branch for the release-tag step to resolve a
  # head SHA. Ignored when importing an existing (already-initialized) repo.
  auto_init = true
  topics    = each.value.topics

  lifecycle {
    prevent_destroy = true
    ignore_changes = [
      description,
      homepage_url,
      topics,
    ]
  }
}

locals {
  # Every expansion repo gets the same CI secrets (var.shared_repository_secrets).
  # Flatten into a single keyed map without exposing secret values in resource
  # keys. Replaces the old per-repo var.repository_secrets triple — adding an
  # expansion no longer means re-declaring its secrets.
  repository_secret_pairs = {
    for pair in flatten([
      for slug in keys(local.expansions) : [
        for secret_name, secret_value in nonsensitive(var.shared_repository_secrets) : {
          key        = "${slug}:${secret_name}"
          repository = slug
          name       = secret_name
          value      = secret_value
        }
      ]
    ]) : pair.key => pair
  }
}

resource "github_actions_secret" "repository" {
  for_each = local.repository_secret_pairs

  repository      = github_repository.managed[each.value.repository].name
  secret_name     = each.value.name
  plaintext_value = each.value.value
}

# Render the server's DB catalogue seed from local.expansions. Committed so CI
# bakes it into the server image; the server applies SEED_DIR at boot. After
# editing expansions.tf, run `terraform apply` and commit the regenerated file.
resource "local_file" "expansions_seed" {
  filename = "${path.module}/../../backend/seed/0002_expansions.generated.sql"
  content = templatefile("${path.module}/templates/expansions_seed.sql.tftpl", {
    expansions = local.expansions
  })
}
