variable "region" {
  description = "AWS region to deploy into"
  type        = string
  default     = "us-east-1"
}

variable "instance_type" {
  description = "EC2 instance type"
  type        = string
  default     = "c7i.large"
}

variable "ssh_public_key" {
  description = "SSH public key material for the EC2 key pair (sourced from the 1Password vault; see DEPLOY.md). OpenSSH single-line format."
  type        = string
}

variable "ssh_allowed_cidr" {
  description = "CIDR allowed to SSH to the host"
  type        = string
  default     = "0.0.0.0/0"
}

variable "allowed_cidrs" {
  description = "CIDRs allowed to reach game services"
  type        = list(string)
  default     = ["0.0.0.0/0"]
}

variable "vpc_cidr" {
  description = "The VPC's range (network.tf carves the public and private subnets from it). Prod's VPC was made with the default."
  type        = string
  default     = "10.44.0.0/16"
}

variable "azs" {
  description = "The two availability zones, [a, b]: public and private subnet a in the first, b in the second. Empty (the default): the region's first two. Pin them in the deployment's var-file once the stack exists, so a change in the region's list never moves a subnet (a moved subnet is a new one)."
  type        = list(string)
  default     = []

  validation {
    condition     = length(var.azs) == 0 || length(var.azs) == 2
    error_message = "azs takes exactly two zones, or none."
  }
}

variable "instance_name" {
  description = "Base name tag for resources."
  type        = string
  default     = "opennova-server"
}

variable "gate_udp_port" {
  description = "UDP port for the novaworld_gate listener"
  type        = number
  default     = 7597
}

variable "nw_udp_port" {
  description = "UDP port for the NovaWorld UDP listener"
  type        = number
  default     = 64206
}

variable "game_http_port" {
  description = "TCP port for the game-facing HTTP service (the gate response carries this port in its startup URL, so it does not need to be 80)"
  type        = number
  default     = 8080
}

variable "web_http_port" {
  description = "HTTP port for the user-facing web frontend"
  type        = number
  default     = 80
}

variable "web_https_port" {
  description = "HTTPS port for the user-facing web frontend"
  type        = number
  default     = 443
}

variable "ami_id" {
  description = "Override AMI ID. Leave blank to auto-discover Ubuntu 24.04. Pin this after the first prod apply so AMI drift never forces instance replacement."
  type        = string
  default     = ""
}

variable "allocate_eip" {
  description = "Whether to allocate and associate an Elastic IP."
  type        = bool
  default     = true
}

variable "tags" {
  description = "Additional resource tags"
  type        = map(string)
  default     = {}
}

# Cloudflare DNS configuration
variable "cloudflare_api_token" {
  description = "Cloudflare API token with DNS edit permissions (sourced from the vault as TF_VAR_cloudflare_api_token)"
  type        = string
  default     = ""
  sensitive   = true
}

variable "cloudflare_zone_id" {
  description = "Cloudflare Zone ID for the domain"
  type        = string
  default     = ""
}

variable "cloudflare_domain" {
  description = "Domain name (e.g. 'example.com'). Creates the @ and www records."
  type        = string
  default     = ""
}

variable "edge_auth_secret" {
  description = "Edge authentication for the portal's /api/ (DEPLOY.md): when set, and while the web records are proxied (cloudflare_web_proxied), a Cloudflare Transform Rule adds X-OpenNova-Edge: <secret> to every request on the web hostnames, and the web container (EDGE_AUTH_SECRET, the same value) answers /api/ only to requests carrying it. Letters and digits, at least 32 (web/docker-entrypoint.sh's rule); lowercase hex in practice (openssl rand -hex 32), since nginx compares it without regard to case. Empty (the default) creates no rule. Sourced from the vault as TF_VAR_edge_auth_secret once enabled; the API token then needs Zone / Transform Rules / Edit. The state holds it in plain text."
  type        = string
  default     = ""
  sensitive   = true

  validation {
    condition     = var.edge_auth_secret == "" || can(regex("^[A-Za-z0-9]{32,}$", var.edge_auth_secret))
    error_message = "edge_auth_secret must be empty or at least 32 letters and digits (the rule web/docker-entrypoint.sh applies to EDGE_AUTH_SECRET)."
  }
}

variable "cloudflare_web_proxied" {
  description = "Whether the web records (@ / www) are proxied through Cloudflare. Proxied gives the site Cloudflare TLS."
  type        = bool
  default     = true
}

variable "backup_bucket_name" {
  description = "S3 bucket for automated database backups."
  type        = string
  default     = "opennova-backups"
}

# ---------------------------------------------------------------------------
# Hosted game servers (hosted.tf; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------

variable "hosted_data_snapshots" {
  description = "The data sets hosted game servers run, each the games slug of the lobby it lists in (jop_2_consumer, dfx2_consumer, opennova_0_consumer) to the private, encrypted EBS snapshot holding its game data, which its machines mount read-only (made once per data set by `./deploy/run.sh hosted data-snapshot --data-set <slug>`). One launch template per entry. Empty (the default) leaves hosting off: hosted.tf creates nothing and changes nothing. The toolbox reads it from the vault (item `hosted`, one field `data_snapshot_id_<slug>` per data set) as TF_VAR_hosted_data_snapshots."
  type        = map(string)
  default     = {}

  validation {
    condition     = alltrue([for slug in keys(var.hosted_data_snapshots) : contains(["jop_2_consumer", "dfx2_consumer", "opennova_0_consumer"], slug)])
    error_message = "hosted_data_snapshots takes the data sets jop_2_consumer, dfx2_consumer and opennova_0_consumer alone."
  }
  validation {
    condition     = alltrue([for id in values(var.hosted_data_snapshots) : can(regex("^snap-[0-9a-f]{8,17}$", id))])
    error_message = "every hosted_data_snapshots value must be a snapshot id (snap-...)."
  }
}

variable "hosted_instance_type" {
  description = "The hosted game server machines' type: one machine per server. Never a burstable type (t2/t3/t4g...), whose spent CPU credits would starve the 62.5 Hz tick."
  type        = string
  default     = "c7a.medium"

  validation {
    condition     = !can(regex("^t[0-9]", var.hosted_instance_type))
    error_message = "hosted_instance_type must not be a burstable t-family type."
  }
}

variable "hosted_ami_id" {
  description = "Override the hosted machines' image (an Ubuntu 24.04 x86_64 AMI). Empty: Canonical's newest when a launch template is made; later images wait for `terraform apply -replace='aws_launch_template.hosted[\"<data set>\"]'`, one per data set."
  type        = string
  default     = ""
}

variable "hosted_root_volume_gb" {
  description = "The hosted machines' root volume (the OS, docker, the serve image and the server's working directory), in GiB."
  type        = number
  default     = 10
}

variable "hosted_deployment" {
  description = "The opennova:deployment tag every hosted machine carries and the service's every EC2 call is scoped to, so two stacks in one account never touch each other's machines. Empty: instance_name."
  type        = string
  default     = ""

  validation {
    condition     = can(regex("^[A-Za-z0-9._-]{0,64}$", var.hosted_deployment))
    error_message = "hosted_deployment takes up to 64 letters, digits, '.', '_' and '-'."
  }
}
