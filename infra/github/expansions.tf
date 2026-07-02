# Single source of truth for game expansions.
#
# Adding an expansion is one edit here + `terraform apply`: this map drives
#   - the GitHub content repository (github_repository.managed, main.tf)
#   - that repo's Actions secrets (github_actions_secret.repository, main.tf)
#   - the server's DB catalogue seed (backend/seed/0002_expansions.generated.sql,
#     rendered by local_file.expansions_seed in main.tf), which carries
#     github_repo so the admin release handler reads the slug->repo mapping from
#     the DB instead of a hardcoded map.
#
# No C++, SQL, or web edits are needed per expansion — the server reads the
# catalogue from the DB and the web reads it from /api/expansions.

locals {
  expansions = {
    revx02 = {
      github_repo    = "${var.github_owner}/revx02"
      game_slug      = "jop_2_consumer"
      display_name   = "Tactical Assault Coalition"
      summary        = "Coalition expansion package for Joint Operations"
      version        = "2024.05.15"
      package_type   = "zip"
      install_subdir = "expansions/revx02"
      featured       = true
      description    = "NovaWorld expansion: RevX02 content package."
      topics         = ["novaworld", "opennova", "expansion"]
    }
    onjo01 = {
      github_repo    = "${var.github_owner}/onjo01"
      game_slug      = "jop_2_consumer"
      display_name   = "OpenNova Demo Mod"
      summary        = "OpenNova remaster expansion for Joint Operations."
      version        = "0.0.0"
      package_type   = "zip"
      install_subdir = "expansion/onjo01"
      featured       = false
      description    = "NovaWorld expansion: Joint Operations content package."
      topics         = ["novaworld", "opennova", "expansion"]
    }
    ondx01 = {
      github_repo    = "${var.github_owner}/ondx01"
      game_slug      = "dfx2_consumer"
      display_name   = "OpenNova Demo Mod"
      summary        = "OpenNova remaster expansion for Delta Force Xtreme 2."
      version        = "0.0.0"
      package_type   = "zip"
      install_subdir = "expansion/ondx01"
      featured       = false
      description    = "NovaWorld expansion: DFX2 content package."
      topics         = ["novaworld", "opennova", "expansion"]
    }
  }
}
