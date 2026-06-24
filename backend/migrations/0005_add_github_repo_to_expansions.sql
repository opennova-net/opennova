-- Per-expansion GitHub repo (owner/repo) the release pipeline tags. Previously
-- a hardcoded slug->repo map lived in the server (server_config.cpp) and onnet's
-- admin.py:19-23. It now lives here, sourced from the single Terraform-managed
-- catalogue (infra/github local.expansions -> backend/seed/0002_expansions.generated.sql),
-- and the admin release handler reads expansions.github_repo per release.
--
-- Forward-only: this column is NOT in 0001_initial.sql's CREATE TABLE, so it
-- applies exactly once on both fresh and existing databases.

ALTER TABLE expansions ADD COLUMN github_repo TEXT;
