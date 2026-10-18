# Staging's Terraform variables: the NON-SECRET knobs (deployment.tfvars.example
# names them all). The secrets, the account and the account's own resource ids
# come from the vault at run time (the toolbox passes them and the deployment:
# ./deploy/run.sh --deployment staging infra ...). Staging gets an Elastic IP
# of its own.

# The service's programs (programs.tf) keep prod's shape: a task each.

azs = ["us-east-1a", "us-east-1b"]

cloudflare_web_proxied = true

# The database (database.tf) and the cache (cache.tf): prod's shape, the defaults.

# Hosted game servers (fleet.tf; DEPLOY.md "Hosted game servers"), rehearsed
# here first: one shared machine is enough.
hosted_enabled   = true
fleet_max_shared = 1
fleet_max_own    = 1

# Staging's account's budgets (observability.tf): its service tier runs about
# $145 a month, about $70 while `app down`.
budget_monthly_usd = 200
budget_egress_gb   = 200
