# 1Password-referenced prod app secrets, injected into the server + web
# containers at deploy time. The non-secret prod values live in
# app.prod.env (committed). `op inject` resolves these 1Password references
# to a tmpfs file; nothing secret is committed or written to the host disk.
ADMIN_API_TOKEN=op://OpenNova-Deploy/app-prod/admin_api_token
ADMIN_BASIC_AUTH_USER=op://OpenNova-Deploy/app-prod/admin_basic_auth_user
ADMIN_BASIC_AUTH_PASSWORD=op://OpenNova-Deploy/app-prod/admin_basic_auth_password
