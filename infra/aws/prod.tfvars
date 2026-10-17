# Prod's Terraform variables: the NON-SECRET knobs (deployment.tfvars.example
# names them all). The secrets, the account and the account's own resource ids
# come from the vault at run time (the toolbox passes them and the deployment:
# ./deploy/run.sh --deployment prod infra ...).

# The service's programs (programs.tf) keep their defaults: a task each, gate,
# lobby and controller at 0.25 vCPU and 0.5 GiB, web and worker at 0.5 vCPU and
# 1 GiB (each scaling up to 2 on its CPU).

# The gate's Elastic IP retail clients reach (54.162.79.147), adopted, never
# managed (the load balancer's zone-a address), is no knob here: its
# allocation id in the prod account (the one `aws ec2 accept-address-transfer`
# gives it at the cutover) comes from prod's vault item (the field
# aws-prod/gate_eip_allocation_id), as TF_VAR_service_eip_allocation_id; a prod
# plan without it stops (service.tf).

azs = ["us-east-1a", "us-east-1b"]

cloudflare_web_proxied = true

# The database (database.tf): Multi-AZ, max_connections sized for
# db_service_processes processes of db_pool_max connections each.
db_instance_class = "db.t4g.small"
db_multi_az       = true

# Outbound mail through Amazon SES (mail.tf): the service sends its welcome
# mail and password reset links as support@<domain>, through prod's own
# identity.
mail_sender = "support"

# Hosted game servers (fleet.tf; DEPLOY.md "Hosted game servers"): the
# machines, the game data's store in the operations bucket, the update
# service. The fleet's sizes keep their defaults.
hosted_enabled = true
