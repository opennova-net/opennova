variable "deployment" {
  description = "The deployment this run is: prod or staging, each in an AWS account of its own. The deploy toolbox passes it (TF_VAR_deployment, from --deployment) beside the deployment's var-file, infra/aws/<deployment>.tfvars."
  type        = string

  validation {
    condition     = contains(["prod", "staging"], var.deployment)
    error_message = "deployment is prod or staging."
  }
}
variable "aws_account_id" {
  description = "The deployment's AWS account (opennova-prod's, opennova-staging's): the provider refuses credentials of any other (allowed_account_ids). The toolbox fills it from the deployment's vault item, aws-<deployment> (TF_VAR_aws_account_id); no tracked file names an account."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.aws_account_id))
    error_message = "aws_account_id is a 12-digit AWS account id."
  }
}

variable "region" {
  description = "The deployment's AWS region. The toolbox fills it from the deployment's vault item, aws-<deployment> (TF_VAR_region)."
  type        = string
}

# ---------------------------------------------------------------------------
# The service's five programs (programs.tf): each its own image and an ECS
# service on Fargate (x86_64). Each takes its task's size and its service's
# task counts; a field left out keeps its default (`web = { max = 6 }` in a
# var-file). cpu is in CPU units (1024 a vCPU), memory in MiB, a Fargate pair
# (256 with 512, 1024 or 2048; 512 with 1024 to 4096; 1024 with 2048 to
# 8192). desired is the count a new service starts with; min and max are the
# bounds Application Auto Scaling holds the count within (web and the worker
# scale on their CPU between them; the others stay where they start, 1 by
# default), and max, with a deployment's extra tasks, is what the database's
# max_connections is sized for.
# ---------------------------------------------------------------------------

variable "gate" {
  description = "The gate: UDP gate_udp_port behind the public network load balancer (the hosts' status blobs land there too). Stateless: two run side by side through a deployment."
  type = object({
    cpu     = optional(number, 256)
    memory  = optional(number, 512)
    min     = optional(number, 1)
    max     = optional(number, 1)
    desired = optional(number, 1)
  })
  default = {}
}

variable "lobby" {
  description = "The lobby: one lobby instance per task, UDP nw_udp_port behind the load balancer at the gate's address, which every lobby announces (the gate hands it out) and which spreads the clients across the tasks. More tasks, more lobby instances. A deployment starts the new task beside the old, then the balancer drains the old (10 s) and ECS stops it."
  type = object({
    cpu     = optional(number, 256)
    memory  = optional(number, 512)
    min     = optional(number, 1)
    max     = optional(number, 1)
    desired = optional(number, 1)
  })
  default = {}
}

variable "web" {
  description = "Web: the retail HTTP routes and the website's API (game_http_port) and the portal's nginx (443) in one task, behind the public network load balancer. Scales on its CPU between min and max; two or more keep the site up through a zone's loss."
  type = object({
    cpu     = optional(number, 512)
    memory  = optional(number, 1024)
    min     = optional(number, 1)
    max     = optional(number, 2)
    desired = optional(number, 1)
  })
  default = {}
}

variable "controller" {
  description = "The controller: the fleet's reconcile pass, the jobs' scheduling and the internal listener the machines' hostd reach at its Cloud Map name. Elected: one leads, another waits as its standby (a deployment's new task does, then leads)."
  type = object({
    cpu     = optional(number, 256)
    memory  = optional(number, 512)
    min     = optional(number, 1)
    max     = optional(number, 1)
    desired = optional(number, 1)
  })
  default = {}
}

variable "worker" {
  description = "The worker: the background jobs (the fleet's AWS calls, billing sync, mail, prunes). Scales on its CPU between min and max: more tasks, more jobs at once."
  type = object({
    cpu     = optional(number, 512)
    memory  = optional(number, 1024)
    min     = optional(number, 1)
    max     = optional(number, 2)
    desired = optional(number, 1)
  })
  default = {}
}

variable "image_registry" {
  description = "Where the service's images are published: <registry>/opennova-novaworld-<program>, <registry>/novaworld-web (the portal's nginx), <registry>/opennova-novaworld-backup, and the machines' <registry>/opennova-serve (.github/workflows/novaworld-images.yml, serve-image.yml). Public packages: the tasks and the machines pull anonymously."
  type        = string
  default     = "ghcr.io/opennova-net"

  validation {
    condition     = can(regex("^[a-z0-9.-]+(:[0-9]+)?(/[a-z0-9._-]+)+$", var.image_registry))
    error_message = "image_registry is a registry host and path (ghcr.io/<owner>), lowercase, with no trailing slash."
  }
}

variable "image_tag" {
  description = "The tag the task definitions Terraform makes name, the templates `app deploy` clones: a deploy replaces every image with the release's, pinned by digest, so this only matters for a service's first tasks before the first deploy."
  type        = string
  default     = "latest"
}

variable "health_port" {
  description = "The port every program answers GET /livez on (ONNET_HEALTH_PORT): the container health checks', and the load balancer's check of the gate. Web also answers /livez and /healthz on game_http_port."
  type        = number
  default     = 8081
}

variable "allowed_cidrs" {
  description = "CIDRs allowed to reach the retail ports (the gate and the HTTP routes at the load balancer, the lobby's UDP port) and the game machines' slots"
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

variable "gate_udp_port" {
  description = "UDP port of the gate (the fixed port of stock clients), at the load balancer and on the gate's tasks"
  type        = number
  default     = 7597
}

variable "nw_udp_port" {
  description = "UDP port of the lobby (the one the gate announces), at the load balancer and on each lobby task"
  type        = number
  default     = 64206
}

variable "game_http_port" {
  description = "TCP port of web's HTTP routes, at the load balancer and on web's tasks (the gate's startup URL carries it, so it does not need to be 80). The portal's nginx (web/nginx.conf) reaches web's server on 127.0.0.1:8080, for /api/ and the retail routes alike, so another value needs its proxy_pass lines changed with it"
  type        = number
  default     = 8080
}

variable "service_eip_allocation_id" {
  description = "The gate's Elastic IP, by allocation id (eipalloc-...): the load balancer's address in zone a (its subnet mapping there), where retail clients reach the gate and web's HTTP routes (ONNET_PUBLIC_HOST, ONNET_WEB_HOST). Prod adopts the address retail clients already use (transferred into the prod account, where it has an allocation id of its own), which Terraform then reads and never manages, so no plan can release it; prod requires it, from its vault item (TF_VAR_service_eip_allocation_id, the field aws-prod/gate_eip_allocation_id: an account's resource id is in no tracked file). Staging leaves it empty and gets an address of its own, managed here."
  type        = string
  default     = ""

  validation {
    condition     = var.service_eip_allocation_id == "" || can(regex("^eipalloc-[0-9a-f]{8,17}$", var.service_eip_allocation_id))
    error_message = "service_eip_allocation_id must be empty or an allocation id (eipalloc-...)."
  }
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
  description = "Cloudflare API token, with the permissions DEPLOY.md lists (section 1; this stack uses DNS and Origin CA: the zone's settings and rules are infra/zone's). From the vault as TF_VAR_cloudflare_api_token (item cloudflare, field api_token)."
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
  description = "The zone's domain (e.g. 'example.com'). Prod makes the @ and www records; staging staging.<domain>."
  type        = string
  default     = ""
}

variable "cloudflare_web_proxied" {
  description = "Whether the web records are proxied through Cloudflare: the site's TLS, its origin certificate, edge authentication. The portal's port admits Cloudflare's ranges alone, so unproxied records reach nothing."
  type        = bool
  default     = true
}

variable "email_forward_to" {
  description = "The maintainer's mailbox (the vault's email/forward_to, TF_VAR_email_forward_to), which this deployment verifies as an SES identity while mail is on (mail.tf): the recipient the account's SES sandbox lets mail through to. Empty or REPLACE_ME: no such identity. (Cloudflare Email Routing's forwarding to it is infra/zone's.)"
  type        = string
  default     = ""

  validation {
    condition     = contains(["", "REPLACE_ME"], var.email_forward_to) || can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.email_forward_to))
    error_message = "email_forward_to must be an email address, or empty or REPLACE_ME."
  }
}

variable "mail_sender" {
  description = "The local part the service sends its mail as through Amazon SES (mail.tf): \"support\" sends as support@<the deployment's web host> (prod support@<domain>, staging support@staging.<domain>), through the deployment's own SES identity. Empty: no outbound mail (no SES identity, no records, the service's mail off)."
  type        = string
  default     = ""

  validation {
    condition     = var.mail_sender == "" || can(regex("^[a-z0-9]([a-z0-9._+-]{0,62}[a-z0-9])?$", var.mail_sender))
    error_message = "mail_sender is a lowercase local part (letters, digits, '.', '_', '+' and '-', not at either end), at most 64, or empty (mail off)."
  }
}

# ---------------------------------------------------------------------------
# The database (database.tf)
# ---------------------------------------------------------------------------

variable "db_instance_class" {
  description = "The database's instance class (PostgreSQL 17 on RDS). db.t4g.small (2 vCPU, 2 GiB) by default: burstable, and RDS runs its T classes in unlimited mode, so a load past the baseline spends surplus credits, billed by the vCPU-hour, and is never throttled; the db-cpu-credits alarm says when the balance runs low."
  type        = string
  default     = "db.t4g.small"

  validation {
    condition     = can(regex("^db\\.[a-z0-9-]+\\.[a-z0-9]+$", var.db_instance_class))
    error_message = "db_instance_class must be an RDS class (db.<family>.<size>)."
  }
}

variable "db_multi_az" {
  description = "Whether the database runs Multi-AZ: a synchronous standby in the other private subnet's zone, which RDS fails over to on its own (a minute or two). Off saves half the instance's cost and gives up the standby."
  type        = bool
  default     = true
}

variable "db_pool_max" {
  description = "The most database connections one program's process opens (DATABASE_POOL_MAX in its task's environment). max_connections is sized from it and db_service_processes. A worker holds one for each of its job threads (ONNET_WORKER_THREADS, 4), its keeper and its main loop, the most of any program, and runs at most DATABASE_POOL_MAX less two job threads: at least 6. The others lease one while they work."
  type        = number
  default     = 7

  validation {
    condition     = var.db_pool_max >= 6 && var.db_pool_max <= 64 && floor(var.db_pool_max) == var.db_pool_max
    error_message = "db_pool_max must be a whole number 6..64 (a worker's 4 job threads, its keeper and its main loop hold one each)."
  }
}

variable "db_service_processes" {
  description = "The most program processes connected to the database at once: every program's tasks at its max, and the extra tasks a deployment runs beside them (a deployment's maximum percent, 200: every program starts its new tasks first), so 2 + 2 + 4 + 2 + 4 = 14 at the defaults; the plan refuses less than that. The database's max_connections is db_pool_max for each of them, plus 10 for the backup task, the operator's psql and the reserved superuser and RDS connections. Changing it is a parameter group change RDS applies at the next reboot."
  type        = number
  default     = 14

  validation {
    condition     = var.db_service_processes >= 1 && var.db_service_processes <= 64 && floor(var.db_service_processes) == var.db_service_processes
    error_message = "db_service_processes must be a whole number 1..64."
  }
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
  description = "The master user's password, from the vault (TF_VAR_db_master_password: item rds-<deployment>; `secrets init` makes it). Write-only: it never enters the state or a plan, and it reaches RDS only when db_master_password_version changes (and at creation)."
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
# The cache (cache.tf)
# ---------------------------------------------------------------------------

variable "cache_node_type" {
  description = "The cache's node type (ElastiCache for Valkey; two nodes, a primary and its replica in the other zone). Web's sessions, pins and rate limits are small: cache.t4g.micro holds about half a GiB."
  type        = string
  default     = "cache.t4g.micro"

  validation {
    condition     = can(regex("^cache\\.[a-z0-9-]+\\.[a-z0-9]+$", var.cache_node_type))
    error_message = "cache_node_type must be an ElastiCache node type (cache.<family>.<size>)."
  }
}

variable "cache_auth_token" {
  description = "The cache's AUTH token, from the vault (TF_VAR_cache_auth_token: item app-<deployment>, field redis_password; `secrets init` makes it), the value web gets as ONNET_REDIS_PASSWORD (secrets sync). The state holds it."
  type        = string
  sensitive   = true

  validation {
    condition     = can(regex("^[A-Za-z0-9]{16,128}$", var.cache_auth_token))
    error_message = "cache_auth_token must be 16 to 128 letters and digits (secrets init makes 64 hex digits)."
  }
}

# ---------------------------------------------------------------------------
# The fleet: hosted game servers' machines (fleet.tf; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------

variable "hosted_enabled" {
  description = "Whether hosted game servers are on (DEPLOY.md \"Hosted game servers\"). On: fleet.tf makes the machines' launch templates, image parameter, security group and roles, programs.tf gives web and the worker the game data's store (the deployment's operations bucket under data/, which admins fill from the site's Game data page, ADR 0064), and the programs' environment names the ec2 provider and the store. Off (the default): fleet.tf creates nothing and the programs run with hosting off. The role boundary must hold the store's S3 actions before the apply that turns it on (infra/aws/iam/README.md: the org stack applies it)."
  type        = bool
  default     = false
}

variable "update_ports" {
  description = "The TCP ports retail UPDATE.EXE reaches an update service on, hardcoded in the updater (docs/update/updater-re.md): the load balancer listens on each, forwarding to web, which serves them all (ONNET_UPDATE_PORTS). The updater's are fixed; another set is only for a stack serving another client."
  type        = list(number)
  default     = [20044, 20556, 21068, 21580]

  validation {
    condition     = length(var.update_ports) >= 1 && length(distinct(var.update_ports)) == length(var.update_ports) && alltrue([for p in var.update_ports : p >= 1024 && p <= 65535 && floor(p) == p])
    error_message = "update_ports takes at least one port, each a whole number 1024..65535, none repeated."
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
  description = "Override the fleet's seed image (an Ubuntu 24.04 x86_64 AMI), what the machines launch from until `fleet bake-image` bakes one. Empty: Canonical's newest when the image parameter is made; a later seed waits for a -replace of terraform_data.fleet_seed_image[0] and aws_ssm_parameter.fleet_image[0]."
  type        = string
  default     = ""
}

variable "machine_root_volume_gb" {
  description = "The machines' root volume, in GiB: the OS, docker, the serve image, the slots' working directories, and the data trees hostd fetches from the store under /opt/game (a tree per data set version its servers pin, about 2 GiB for JO's; a shared machine holds several at once)."
  type        = number
  default     = 40
}

variable "internal_http_port" {
  description = "The controller's internal listener's port, on its task's private address (the leader alone binds it; ONNET_INTERNAL_URL names it by its Cloud Map name): the machines' hostd bootstrap and sync, admitted from the machines' security group alone."
  type        = number
  default     = 8070
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
  description = "The deployment's monthly cost budget (its account's, which holds it alone), in USD: an email at 80% spent and at a 100% forecast. Sized for prod's service tier (the five programs on Fargate, the load balancer, a Multi-AZ db.t4g.small and the cache, about $190: DEPLOY.md, \"Cost\") and a few machines; staging.tfvars sets its own."
  type        = number
  default     = 350
}

variable "budget_egress_gb" {
  description = "The deployment's (its account's) monthly EC2 data-transfer-out budget, in GB (the hosted servers' egress is metered, not capped): an email at 80% used and at a 100% forecast."
  type        = number
  default     = 1500
}

variable "cost_anomaly_monitor" {
  description = "Whether the deployment makes its account's Cost Anomaly Detection monitor (by AWS service). An account holds one such monitor: false if it has one already (or import it)."
  type        = bool
  default     = true
}

variable "cost_anomaly_threshold_usd" {
  description = "A cost anomaly's total impact, in USD, from which it is emailed (daily)."
  type        = number
  default     = 20
}
