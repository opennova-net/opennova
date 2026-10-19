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
# The static address add-on (ADR 0066), rehearsed here first: two, within the
# account's default Elastic IP quota of 5 beside the load balancer's two.
fleet_max_addresses = 2
# The machines' image, pinned by its exact name (fleet_agent.tf): Amazon's
# ECS-optimized Amazon Linux 2023. A bump lands here, staging's first;
# the newest: aws ssm get-parameters --names
# /aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_name
fleet_ami = "al2023-ami-ecs-hvm-2023.0.20261009-kernel-6.1-x86_64"

# Staging's account's budgets (observability.tf): its service tier runs about
# $160 a month, about $70 while `app down` (DEPLOY.md, "Cost").
budget_monthly_usd = 200
budget_egress_gb   = 200
