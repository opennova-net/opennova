# 1Password-referenced prod app secrets, injected into the server + web
# containers at deploy time. The non-secret prod values live in
# app.prod.env (committed). `op inject` resolves these 1Password references
# to a tmpfs file; nothing secret is committed or written to the host disk.
ADMIN_API_TOKEN=op://OpenNova-Deploy/app-prod/admin_api_token
ADMIN_BASIC_AUTH_USER=op://OpenNova-Deploy/app-prod/admin_basic_auth_user
ADMIN_BASIC_AUTH_PASSWORD=op://OpenNova-Deploy/app-prod/admin_basic_auth_password
# Expansion-publish pipeline. expansion_github_token is a GitHub PAT with
# contents:write on the expansion repos (the server pushes a release tag);
# expansion_publish_token is the bearer the expansion repo's CI presents to
# /admin/internal/.../publish. The SAME publish-token value is set on each
# expansion repo as an Actions secret by the infra/github stack.
EXPANSION_GITHUB_TOKEN=op://OpenNova-Deploy/app-prod/expansion_github_token
EXPANSION_PUBLISH_TOKEN=op://OpenNova-Deploy/app-prod/expansion_publish_token
