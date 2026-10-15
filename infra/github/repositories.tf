# Every repository in the organization, as each stood when the stack adopted it.
# A repository's settings are the defaults below, overridden per repository.
# Never destroyed by Terraform: prevent_destroy refuses a plan that would, and
# a repository removed from this map is archived (archive_on_destroy), never
# deleted, once prevent_destroy is lifted on purpose.
locals {
  repository_defaults = {
    description            = null
    homepage_url           = null
    visibility             = "private"
    topics                 = []
    has_issues             = true
    has_projects           = true
    has_wiki               = true
    has_discussions        = false
    vulnerability_alerts   = false
    default_branch         = "main"
    fork                   = false
    source_owner           = null
    source_repo            = null
    allow_merge_commit     = true
    allow_squash_merge     = true
    allow_rebase_merge     = true
    allow_auto_merge       = false
    delete_branch_on_merge = false
    allow_update_branch    = false
  }

  repositories = {
    for name, settings in {
      ".github" = {
        visibility           = "public"
        vulnerability_alerts = true
      }
      opennova = {
        description          = "A reimplementation of Novalogic's game engine"
        homepage_url         = "https://opennova.net"
        visibility           = "public"
        topics               = ["bhd", "joint-operations", "novalogic", "novalogic-games", "reimplementation", "reimplemented-engine", "reverse-engineering"]
        vulnerability_alerts = true
        default_branch       = "master"
      }
      modsuperoed = {
        visibility           = "public"
        vulnerability_alerts = true
      }
      WolfRAT2 = {
        description    = "WolfRAT2 — Modern Joint Operations Server Admin Tool. PyQt6 replacement for the original WolfRAT v0.95 (2005)."
        visibility     = "public"
        has_issues     = false
        default_branch = "master"
        fork           = true
        source_owner   = "BadgerLove"
        source_repo    = "WolfRAT2"
      }
      docs = {
        description    = "Agent Docs"
        default_branch = "master"
      }
      opennova-ida = {
        default_branch = "master"
      }
      opennova-int = {}
      opennova-int-bhd = {
        default_branch = "master"
      }
      opennova-reference-assets        = {}
      opennova-reference-retail-packed = {}
    } : name => merge(local.repository_defaults, settings)
  }
}

resource "github_repository" "repo" {
  for_each = local.repositories

  name         = each.key
  description  = each.value.description
  homepage_url = each.value.homepage_url
  visibility   = each.value.visibility
  topics       = each.value.topics
  fork         = each.value.fork
  source_owner = each.value.source_owner
  source_repo  = each.value.source_repo

  has_issues      = each.value.has_issues
  has_projects    = each.value.has_projects
  has_wiki        = each.value.has_wiki
  has_discussions = each.value.has_discussions

  allow_merge_commit          = each.value.allow_merge_commit
  allow_squash_merge          = each.value.allow_squash_merge
  allow_rebase_merge          = each.value.allow_rebase_merge
  allow_auto_merge            = each.value.allow_auto_merge
  delete_branch_on_merge      = each.value.delete_branch_on_merge
  allow_update_branch         = each.value.allow_update_branch
  squash_merge_commit_title   = "COMMIT_OR_PR_TITLE"
  squash_merge_commit_message = "COMMIT_MESSAGES"
  merge_commit_title          = "MERGE_MESSAGE"
  merge_commit_message        = "PR_TITLE"
  web_commit_signoff_required = false

  archive_on_destroy = true

  lifecycle {
    prevent_destroy = true
  }
}

resource "github_branch_default" "repo" {
  for_each   = local.repositories
  repository = github_repository.repo[each.key].name
  branch     = each.value.default_branch
}

resource "github_repository_vulnerability_alerts" "repo" {
  for_each   = local.repositories
  repository = github_repository.repo[each.key].name
  enabled    = each.value.vulnerability_alerts
}
