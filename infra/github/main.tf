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
  }
}

provider "github" {
  token = var.github_token
  owner = var.github_owner
}

locals {
  # The expansion content packages. Their build workflows tag -> package ->
  # upload to S3 -> call the server's /admin/internal/.../publish callback.
  repositories = {
    revx02 = {
      description = "NovaWorld expansion: RevX02 content package."
      visibility  = "private"
      topics      = ["novaworld", "opennova", "expansion"]
    }
    onjo01 = {
      description = "NovaWorld expansion: Joint Operations content package."
      visibility  = "private"
      topics      = ["novaworld", "opennova", "expansion"]
    }
    ondx01 = {
      description = "NovaWorld expansion: DFX2 content package."
      visibility  = "private"
      topics      = ["novaworld", "opennova", "expansion"]
    }
  }
}

resource "github_repository" "managed" {
  for_each = local.repositories

  name                   = each.key
  description            = lookup(each.value, "description", null)
  visibility             = lookup(each.value, "visibility", "private")
  has_issues             = lookup(each.value, "has_issues", true)
  has_projects           = lookup(each.value, "has_projects", false)
  has_wiki               = lookup(each.value, "has_wiki", false)
  allow_merge_commit     = lookup(each.value, "allow_merge_commit", true)
  allow_rebase_merge     = lookup(each.value, "allow_rebase_merge", true)
  allow_squash_merge     = lookup(each.value, "allow_squash_merge", true)
  delete_branch_on_merge = lookup(each.value, "delete_branch_on_merge", true)
  vulnerability_alerts   = lookup(each.value, "vulnerability_alerts", true)
  archive_on_destroy     = true
  # A fresh repo needs a default branch for the release-tag step to resolve a
  # head SHA. Ignored when importing an existing (already-initialized) repo.
  auto_init    = lookup(each.value, "auto_init", true)
  topics       = lookup(each.value, "topics", [])
  homepage_url = lookup(each.value, "homepage_url", null)

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
  # Flatten var.repository_secrets (map of repo -> map of secret -> value) into
  # a single keyed map without exposing secret values in resource keys.
  repository_secret_pairs = {
    for pair in flatten([
      for repo_name, secrets in nonsensitive(var.repository_secrets) : [
        for secret_name, secret_value in secrets : {
          key        = "${repo_name}:${secret_name}"
          repository = repo_name
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
