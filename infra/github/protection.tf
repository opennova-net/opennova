# opennova's master: locked, so only the admins (exempt from the rule) merge
# into it, each merge a pull request with one approval and the two required
# checks (ci.yml's `test (windows-latest)` and `godot-tests (windows-latest)`),
# up to date with master first; being exempt, an admin can merge a docs-only
# pull request at once. Never force-pushed, never deleted.
resource "github_branch_protection" "opennova_master" {
  repository_id = github_repository.repo["opennova"].node_id
  pattern       = "master"

  enforce_admins                  = false
  lock_branch                     = true
  allows_deletions                = false
  allows_force_pushes             = false
  required_linear_history         = false
  require_signed_commits          = false
  require_conversation_resolution = false

  required_status_checks {
    strict   = true
    contexts = ["godot-tests (windows-latest)", "test (windows-latest)"]
  }

  required_pull_request_reviews {
    required_approving_review_count = 1
    dismiss_stale_reviews           = false
    require_code_owner_reviews      = false
  }
}
