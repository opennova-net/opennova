# Staging's Terraform variables: the NON-SECRET knobs (deployment.tfvars.example
# names them all). The secrets, the account and the account's own resource ids
# come from the vault at run time (the toolbox passes them and the deployment:
# ./deploy/run.sh --deployment staging infra ...). Staging gets an Elastic IP
# of its own.

# The service's programs (programs.tf), prod's shape with a second web task, so
# a login whose steps land on different replicas is rehearsed here.
web = { min = 2, desired = 2 }

azs = ["us-east-1a", "us-east-1b"]

cloudflare_web_proxied = true

# The database (database.tf): prod's shape, so a cutover or a failover is
# rehearsed here first.
db_instance_class = "db.t4g.small"
db_multi_az       = true

# Hosted game servers (fleet.tf; DEPLOY.md "Hosted game servers"), rehearsed
# here first: one shared machine is enough.
hosted_enabled   = true
fleet_max_shared = 1
fleet_max_own    = 1

# Staging's account's budgets (observability.tf): its service tier runs about
# $210 a month with its second web task.
budget_monthly_usd = 250
budget_egress_gb   = 200
