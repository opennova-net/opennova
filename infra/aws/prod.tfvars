# Prod's Terraform variables: the NON-SECRET knobs (deployment.tfvars.example
# names them all). The secrets, the account and the account's own resource ids
# come from the vault at run time (the toolbox passes them and the deployment:
# ./deploy/run.sh --deployment prod infra ...).

# The service's programs (programs.tf) keep their defaults: a task each, gate,
# lobby and fleet at 0.25 vCPU and 0.5 GiB, web and worker at 0.5 vCPU and
# 1 GiB, data at 0.25 vCPU and 1 GiB with 40 GiB of task storage.

# The gate's Elastic IP retail clients reach (54.162.79.147), adopted, never
# managed (the load balancer's zone-a address), is no knob here: its
# allocation id in the prod account comes from prod's vault item (the field
# aws-prod/gate_eip_allocation_id; DEPLOY.md, "The var-file"), as
# TF_VAR_service_eip_allocation_id; a prod plan without it stops (service.tf).

azs = ["us-east-1a", "us-east-1b"]

cloudflare_web_proxied = true

# The database (database.tf) and the cache (cache.tf) keep their defaults while
# traffic is small: a db.t4g.micro in one zone and one cache node. Their
# standbys come back with db_multi_az = true and cache_node_count = 2.

# Outbound mail through Amazon SES (mail.tf): the service sends its welcome
# mail and password reset links as support@<domain>, through prod's own
# identity.
mail_sender = "support"

# Hosted game servers (fleet.tf; DEPLOY.md "Hosted game servers"): the
# machines, the game data's store in the operations bucket, the update
# service. The fleet's sizes keep their defaults.
hosted_enabled = true
# The static address add-on (ADR 0066): one for sale, within the account's
# default Elastic IP quota of 5 beside the load balancer's two.
fleet_max_addresses = 1
# The machines' image, pinned by its exact name (fleet_agent.tf): Amazon's
# ECS-optimized Amazon Linux 2023. A bump lands here, staging's first;
# the newest: aws ssm get-parameters --names
# /aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_name
fleet_ami = "al2023-ami-ecs-hvm-2023.0.20261009-kernel-6.1-x86_64"
