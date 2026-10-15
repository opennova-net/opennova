# Staging's Terraform variables: the NON-SECRET knobs (deployment.tfvars.example
# names them all). The secrets come from the vault at run time. Staging gets an
# Elastic IP of its own.

region        = "us-east-1"
instance_type = "c7i.large"

azs = ["us-east-1a", "us-east-1b"]

cloudflare_web_proxied = true

# One shared machine is enough to rehearse on.
fleet_max_shared = 1
fleet_max_own    = 1
