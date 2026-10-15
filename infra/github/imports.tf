# What existed before the stack, adopted rather than made again: the first
# plan imports each of these, and once in the state an import block is a no-op.
# They stay so a state rebuilt from nothing adopts the organization the same way.
# (The runner group and the runners were the stack's own; the secret has no
# value to import, and an apply writes it.)

import {
  to = github_organization_settings.org
  id = "237079256"
}

import {
  for_each = var.members
  to       = github_membership.member[each.key]
  id       = "${var.owner}:${each.key}"
}

import {
  to = github_actions_organization_permissions.org
  id = var.owner
}

import {
  to = github_actions_organization_workflow_permissions.org
  id = var.owner
}

import {
  for_each = local.repositories
  to       = github_repository.repo[each.key]
  id       = each.key
}

import {
  for_each = local.repositories
  to       = github_branch_default.repo[each.key]
  id       = each.key
}

import {
  for_each = local.repositories
  to       = github_repository_vulnerability_alerts.repo[each.key]
  id       = each.key
}

import {
  to = github_branch_protection.opennova_master
  id = "opennova:master"
}
