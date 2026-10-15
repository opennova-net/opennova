# Prod's Terraform variables: the NON-SECRET knobs (deployment.tfvars.example
# names them all). The secrets come from the vault at run time.

region        = "us-east-1"
instance_type = "c7i.large"

# The Elastic IP retail clients reach, adopted, never managed.
service_eip_allocation_id = "eipalloc-05d28b7cdb8ae7c0a"

# Public a keeps the single-host stack's subnet zone.
azs = ["us-east-1a", "us-east-1b"]

cloudflare_web_proxied = true
