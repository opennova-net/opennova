variable "region" {
  description = "AWS region to deploy into"
  type        = string
  default     = "us-east-1"
}

variable "instance_type" {
  description = "The service host's instance type: the NovaWorld server, the portal's nginx and the web build in docker compose. Never a burstable type (the server's loops run all the time)."
  type        = string
  default     = "c7i.large"

  validation {
    condition     = !can(regex("^t[0-9]", var.instance_type))
    error_message = "instance_type must not be a burstable t-family type."
  }
}

variable "allowed_cidrs" {
  description = "CIDRs allowed to reach the retail ports (the gate, NovaWorld UDP, the HTTP routes) and the game machines' slots"
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
  description = "Base name of the resources. Prod's (the default workspace's) resources carry it as it is, another deployment's with its name appended."
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

variable "ami_id" {
  description = "Override the service host's image (an Ubuntu 24.04 x86_64 AMI). Empty: Canonical's newest when the host is made; a later image waits for a deliberate -replace=aws_instance.service."
  type        = string
  default     = ""
}

variable "service_eip_allocation_id" {
  description = "The Elastic IP the service host takes, by allocation id (eipalloc-...): prod adopts the address retail clients already use, which Terraform then reads and never manages, so no plan can release it. Empty: the deployment gets an address of its own, managed here (staging)."
  type        = string
  default     = ""

  validation {
    condition     = var.service_eip_allocation_id == "" || can(regex("^eipalloc-[0-9a-f]{8,17}$", var.service_eip_allocation_id))
    error_message = "service_eip_allocation_id must be empty or an allocation id (eipalloc-...)."
  }
}

variable "service_private_ip_host" {
  description = "The service host's fixed private address, as a host number in public subnet a (10 gives 10.44.0.10). Its network interface keeps it across instances; the game machines reach the service's internal listener there."
  type        = number
  default     = 10

  validation {
    condition     = var.service_private_ip_host >= 4 && var.service_private_ip_host <= 250
    error_message = "service_private_ip_host must be 4..250 (AWS reserves the first four addresses of a subnet and the last)."
  }
}

variable "service_root_volume_gb" {
  description = "The service host's root volume (the OS, docker, the images of a few releases), in GiB."
  type        = number
  default     = 30
}

variable "log_retention_days" {
  description = "How long CloudWatch keeps the service's and the machines' logs, in days."
  type        = number
  default     = 30
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
  description = "Domain name (e.g. 'example.com'). Prod makes the @, www and game records; another deployment <deployment> and game-<deployment>."
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
  description = "Whether the web records are proxied through Cloudflare. Proxied gives the site Cloudflare TLS; the portal's port admits Cloudflare's ranges alone, so unproxied records reach nothing."
  type        = bool
  default     = true
}

variable "ops_bucket_name" {
  description = "The operations bucket: every deployment's releases (releases/<deployment>/) and database dumps (novaworld/ for prod, novaworld-<deployment>/ otherwise). Prod (the default workspace) owns it; another deployment names the same bucket."
  type        = string
  default     = "opennova-backups"
}

# ---------------------------------------------------------------------------
# The database (database.tf)
# ---------------------------------------------------------------------------

variable "db_instance_class" {
  description = "The database's instance class (PostgreSQL 17 on RDS, single-AZ)."
  type        = string
  default     = "db.t4g.micro"
}

variable "db_master_username" {
  description = "The database's master user: `db bootstrap` makes the service's own role and database with it, nothing else uses it."
  type        = string
  default     = "novaworld_admin"

  validation {
    condition     = can(regex("^[a-z][a-z0-9_]{0,62}$", var.db_master_username)) && !contains(["postgres", "rdsadmin", "novaworld"], var.db_master_username)
    error_message = "db_master_username: lowercase letters, digits and '_', not a reserved name or the service's role."
  }
}

variable "db_master_password" {
  description = "The master user's password, from the vault (TF_VAR_db_master_password: item rds, rds-<deployment> off prod; `secrets init` makes it). Write-only: it never enters the state or a plan, and it reaches RDS only when db_master_password_version changes (and at creation)."
  type        = string
  sensitive   = true
  ephemeral   = true

  validation {
    condition     = can(regex("^[A-Za-z0-9]{16,128}$", var.db_master_password))
    error_message = "db_master_password must be 16 to 128 letters and digits (secrets init makes 48 hex digits)."
  }
}

variable "db_master_password_version" {
  description = "Bump it (in the deployment's var-file) to send a changed db_master_password to RDS."
  type        = number
  default     = 1
}

variable "db_name" {
  description = "The service's database (`db bootstrap` makes it, C collation)."
  type        = string
  default     = "novaworld"
}

variable "db_user" {
  description = "The service's own database role (`db bootstrap` makes it; its password is the runtime secret DATABASE_PASSWORD)."
  type        = string
  default     = "novaworld"
}

variable "db_allocated_storage_gb" {
  description = "The database's initial storage, in GiB (gp3, encrypted)."
  type        = number
  default     = 20
}

variable "db_max_allocated_storage_gb" {
  description = "The most the database's storage grows to on its own, in GiB."
  type        = number
  default     = 100
}

variable "db_backup_retention_days" {
  description = "Days of RDS automated backups (point-in-time recovery)."
  type        = number
  default     = 14
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
