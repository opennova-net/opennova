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

variable "vpc_id" {
  description = "Optional VPC ID; leave blank to use an auto-created VPC"
  type        = string
  default     = ""

  validation {
    condition     = (var.vpc_id == "" && var.subnet_id == "") || (var.vpc_id != "" && var.subnet_id != "")
    error_message = "Provide both vpc_id and subnet_id together, or leave both empty to let Terraform create networking."
  }
}

variable "subnet_id" {
  description = "Optional subnet ID; leave blank to use the auto-created subnet"
  type        = string
  default     = ""
}

variable "auto_vpc_cidr" {
  description = "CIDR block for the auto-created VPC when none is provided"
  type        = string
  default     = "10.44.0.0/16"
}

variable "auto_subnet_cidr" {
  description = "CIDR block for the auto-created public subnet"
  type        = string
  default     = "10.44.0.0/24"
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
  description = "Domain name (e.g. 'example.com'). Creates @, www, and nw records."
  type        = string
  default     = ""
}

variable "cloudflare_web_proxied" {
  description = "Whether the web records (@ / www) are proxied through Cloudflare. Proxied gives the site Cloudflare TLS; the nw record is ALWAYS unproxied because game traffic is UDP and the launcher resolves it to a raw IP."
  type        = bool
  default     = true
}

variable "downloads_bucket_name" {
  description = "S3 bucket name for launcher distribution (e.g. downloads.example.com)."
  type        = string
}

variable "backup_bucket_name" {
  description = "S3 bucket for automated database backups."
  type        = string
  default     = "opennova-backups"
}

variable "create_ci_user" {
  description = "Whether to provision an IAM user/access key for GitHub Actions launcher uploads"
  type        = bool
  default     = true
}
