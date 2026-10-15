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
  description = "Edge authentication for the portal's /api/ (DEPLOY.md): a Cloudflare Transform Rule adds X-OpenNova-Edge: <secret> to every request on the web hostnames, and the web container (EDGE_AUTH_SECRET, the same value) answers /api/ only to requests carrying it. Letters and digits, at least 32 (web/docker-entrypoint.sh's rule); lowercase hex in practice (`secrets init` makes 64), since nginx compares it without regard to case. From the vault (TF_VAR_edge_auth_secret: app-prod/edge_auth_secret, the zone's one secret, every deployment's); the API token needs Zone / Transform Rules / Edit. The state holds it in plain text."
  type        = string
  default     = ""
  sensitive   = true

  validation {
    condition     = var.edge_auth_secret == "" || can(regex("^[A-Za-z0-9]{32,}$", var.edge_auth_secret))
    error_message = "edge_auth_secret must be empty or at least 32 letters and digits (the rule web/docker-entrypoint.sh applies to EDGE_AUTH_SECRET)."
  }
}

variable "edge_other_deployments" {
  description = "The deployments besides prod whose web hostnames (<name>.<domain>) the zone's rules cover: edge authentication's header, the webhook's WAF skip. Prod owns the zone's rulesets, so they name every deployment here."
  type        = list(string)
  default     = ["staging"]
}

variable "cloudflare_webhook_skip_phases" {
  description = "What the Stripe webhook's custom rule skips besides the remaining custom rules: these phases. Drop one the zone's plan does not offer (the apply says which)."
  type        = list(string)
  default     = ["http_ratelimit", "http_request_firewall_managed", "http_request_sbfm"]
}

variable "cloudflare_webhook_skip_products" {
  description = "And these legacy security products. Drop one the zone's plan does not offer."
  type        = list(string)
  default     = ["bic", "hot", "securityLevel", "uaBlock", "zoneLockdown"]
}

variable "cloudflare_web_proxied" {
  description = "Whether the web records are proxied through Cloudflare: the site's TLS, its origin certificate, edge authentication. The portal's port admits Cloudflare's ranges alone, so unproxied records reach nothing."
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
# The fleet: hosted game servers' machines (fleet.tf; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------

variable "hosted_data_snapshots" {
  description = "The data sets hosted game servers run, each the games slug of the lobby it lists in (jop_2_consumer, dfx2_consumer, opennova_0_consumer) to the private, encrypted EBS snapshot holding its game data, which every machine mounts read-only, a volume per data set (made once per data set by `./deploy/run.sh hosted data-snapshot --data-set <slug>`). Empty (the default) leaves hosting off: fleet.tf creates nothing. The toolbox reads it from the vault (item `hosted`, one field `data_snapshot_id_<slug>` per data set) as TF_VAR_hosted_data_snapshots."
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

variable "hosted_data_set_missions" {
  description = "Each data set's default map rotation for a new hosted server (ONNET_HOSTED_DATA_SETS in infra.env): .bms file names by data set slug. A data set left out starts with an empty rotation the owner fills in."
  type        = map(list(string))
  default     = {}

  validation {
    condition     = alltrue([for slug in keys(var.hosted_data_set_missions) : contains(keys(var.hosted_data_snapshots), slug)])
    error_message = "hosted_data_set_missions names a data set hosted_data_snapshots does not."
  }
  validation {
    condition     = alltrue(flatten([for m in values(var.hosted_data_set_missions) : [for f in m : can(regex("^[A-Za-z0-9_-]+\\.bms$", f))]]))
    error_message = "every hosted_data_set_missions entry must be a .bms file name of letters, digits, _ and -."
  }
}

variable "shared_instance_type" {
  description = "The shared machines' type: up to shared_slots servers each. A burstable type runs with unlimited CPU credits (the launch template sets them). x86_64 only (the serve image's), and no t2."
  type        = string
  default     = "t3a.large"

  validation {
    condition     = !can(regex("^t2\\.", var.shared_instance_type)) && !can(regex("^[a-z]+[0-9]+[a-z]*g[a-z]*\\.", var.shared_instance_type))
    error_message = "shared_instance_type must be an x86_64 type (no Graviton family such as t4g or c7g) and not t2 (the launch template's unlimited credits are for t3/t3a)."
  }
}

variable "own_instance_type" {
  description = "The own machines' type: exactly one server each (the paid own-machine add-on). The same rules as shared_instance_type."
  type        = string
  default     = "c7a.medium"

  validation {
    condition     = !can(regex("^t2\\.", var.own_instance_type)) && !can(regex("^[a-z]+[0-9]+[a-z]*g[a-z]*\\.", var.own_instance_type))
    error_message = "own_instance_type must be an x86_64 type (no Graviton family such as t4g or c7g) and not t2."
  }
}

variable "shared_slots" {
  description = "K: the servers a shared machine holds (slot k: game UDP 32768+k, admin TCP 32800+k). The service reads it (ONNET_FLEET_SHARED_SLOTS); set it from measuring a full machine."
  type        = number
  default     = 12

  validation {
    condition     = var.shared_slots >= 1 && var.shared_slots <= 32 && floor(var.shared_slots) == var.shared_slots
    error_message = "shared_slots must be 1..32 (the game ports 32768.. must stay below the admin ports 32800..)."
  }
}

variable "fleet_max_shared" {
  description = "The most shared machines the service runs at once (ONNET_FLEET_MAX_SHARED). With fleet_max_own, it sets the vCPU quota the account needs (DEPLOY.md)."
  type        = number
  default     = 2
}

variable "fleet_max_own" {
  description = "The most own machines the service runs at once (ONNET_FLEET_MAX_OWN)."
  type        = number
  default     = 2
}

variable "fleet_idle_minutes" {
  description = "How long a shared machine with no server runs before the service ends it (ONNET_FLEET_IDLE_MINUTES)."
  type        = number
  default     = 30
}

variable "hosted_ami_id" {
  description = "Override the machines' image (an Ubuntu 24.04 x86_64 AMI). Empty: Canonical's newest when a launch template is made; later images wait for a -replace of each template (aws_launch_template.fleet[\"shared\"], [\"own\"])."
  type        = string
  default     = ""
}

variable "machine_root_volume_gb" {
  description = "The machines' root volume (the OS, docker, the serve image and the slots' working directories), in GiB."
  type        = number
  default     = 16
}

variable "internal_http_port" {
  description = "The service's internal listener's port, on its fixed private address (ONNET_INTERNAL_HTTP_BIND): the machines' hostd bootstrap and sync, admitted from the machines' security group alone."
  type        = number
  default     = 8070
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

# ---------------------------------------------------------------------------
# Observability and budgets (observability.tf)
# ---------------------------------------------------------------------------

variable "alert_email" {
  description = "Where the alarms, the budgets and cost anomalies write (an SNS email subscription to confirm once). From the vault (TF_VAR_alert_email: item alerts, field email); empty or REPLACE_ME sends nothing."
  type        = string
  default     = ""
}

variable "budget_monthly_usd" {
  description = "The account's monthly cost budget, in USD: an email at 80% spent and at a 100% forecast."
  type        = number
  default     = 250
}

variable "budget_egress_gb" {
  description = "The account's monthly EC2 data-transfer-out budget, in GB (the hosted servers' egress is metered, not capped): an email at 80% used and at a 100% forecast."
  type        = number
  default     = 1500
}

variable "cost_anomaly_monitor" {
  description = "Whether prod makes the account's Cost Anomaly Detection monitor (by AWS service). An account holds one such monitor: false if it has one already (or import it)."
  type        = bool
  default     = true
}

variable "cost_anomaly_threshold_usd" {
  description = "A cost anomaly's total impact, in USD, from which it is emailed (daily)."
  type        = number
  default     = 20
}

# ---------------------------------------------------------------------------
# The optional GitHub Actions deploy (ci.tf)
# ---------------------------------------------------------------------------

variable "github_deploy" {
  description = "Whether prod makes GitHub's OIDC provider and the role .github/workflows/deploy.yml assumes (DEPLOY.md, section 4). Off by default: deploys run from the toolbox."
  type        = bool
  default     = false
}

variable "github_repository" {
  description = "The repository (owner/name) whose deploy workflow may assume the role."
  type        = string
  default     = "opennova-net/opennova-godot"

  validation {
    condition     = can(regex("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", var.github_repository))
    error_message = "github_repository takes owner/name."
  }
}

variable "github_environment" {
  description = "The repository environment whose jobs alone may assume the role: its required reviewers are the deploy's approval gate."
  type        = string
  default     = "production"
}
