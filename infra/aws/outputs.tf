output "deployment" {
  value       = local.deployment
  description = "The deployment this state is (prod or staging)"
}

output "account_id" {
  value       = local.account_id
  description = "The AWS account the deployment lives in (var.aws_account_id, from the vault)"
}

output "cluster" {
  value       = aws_ecs_cluster.main.name
  description = "The ECS cluster the programs run in"
}

output "services" {
  value       = { for p, s in aws_ecs_service.program : p => s.name }
  description = "Each program's ECS service (app status, app restart PROGRAM)"
}

output "task_definitions" {
  value       = merge({ for p, d in aws_ecs_task_definition.program : p => d.arn }, { backup = aws_ecs_task_definition.backup.arn })
  description = "The task definitions Terraform made, each program's and the backup task's: the templates app deploy clones (infra.env names them too)"
}

output "public_ip" {
  value       = local.service_public_ip
  description = "The gate's public address, the load balancer's in zone a (its Elastic IP): retail clients' gs.novaworld.net, where host status blobs land, and the retail HTTP routes (ONNET_PUBLIC_HOST, ONNET_WEB_HOST)"
}

output "service_eip_allocation_id" {
  value       = local.service_eip_allocation_id
  description = "The gate's Elastic IP's allocation id: prod's var.service_eip_allocation_id (adopted, never managed), or staging's managed address"
}

output "lb_dns_name" {
  value       = aws_lb.public.dns_name
  description = "The public load balancer's DNS name (the site's records point at it)"
}

output "lb_arn" {
  value       = aws_lb.public.arn
  description = "The public load balancer (deletion protection on: DEPLOY.md's rollback from the cutover lifts it first)"
}

output "lb_addresses" {
  value       = { a = local.service_public_ip, b = aws_eip.lb_b.public_ip }
  description = "The public load balancer's static addresses, one per zone (zone a's is the gate's)"
}

output "controller_host" {
  value       = local.controller_host
  description = "The controller's private DNS name (Cloud Map)"
}

output "internal_url" {
  value       = local.internal_url
  description = "The controller's internal listener by its private name (ONNET_INTERNAL_URL): where the machines' hostd bootstrap and sync"
}

output "web_domain" {
  value       = length(cloudflare_record.web_root) > 0 ? cloudflare_record.web_root[0].hostname : null
  description = "The site's hostname"
}

output "update_host" {
  value       = local.update_host != "" ? local.update_host : null
  description = "The update service's hostname, an unproxied record at the load balancer (web's ONNET_UPDATE_HOST; retail UPDATE.EXE reaches it on the update ports); null without a zone"
}

output "ops_bucket" {
  value       = local.ops_bucket_name
  description = "The deployment's operations bucket: the release manifests (releases/), the game data's store (data/: the data sets' trees and the update packages) and the database dumps (novaworld/)"
}

output "ssm_prefix" {
  value       = local.ssm_prefix
  description = "The deployment's SSM parameters: infra.env, release, secret/*, tls/*"
}

output "log_group" {
  value       = aws_cloudwatch_log_group.service.name
  description = "The tasks' logs, a stream per container and task under its program's prefix (app logs)"
}

output "db_endpoint" {
  value       = aws_db_instance.main.address
  description = "The database's hostname (private: the programs reach it; DATABASE_URL in their environment)"
}

output "db_port" {
  value       = aws_db_instance.main.port
  description = "The database's port"
}

output "db_identifier" {
  value       = aws_db_instance.main.identifier
  description = "The database instance (db snapshot; RDS point-in-time restores name it)"
}

output "cache_endpoint" {
  value       = aws_elasticache_replication_group.cache.primary_endpoint_address
  description = "The cache's primary endpoint (private, TLS: ONNET_REDIS_URL in web's environment)"
}

output "cache_port" {
  value       = local.cache_port
  description = "The cache's port"
}

output "vpc_id" {
  value       = aws_vpc.main.id
  description = "The VPC"
}

output "public_subnet_ids" {
  value       = { for k, s in aws_subnet.public : k => s.id }
  description = "The public subnets, a and b (the tasks', the load balancer's and the machines')"
}

output "private_subnet_ids" {
  value       = { for k, s in aws_subnet.private : k => s.id }
  description = "The private subnets, a and b (the database's)"
}

output "region" {
  value       = local.region
  description = "The region everything here lives in (the hosted servers' ONNET_HOSTED_EC2_REGION)"
}

output "hosted_enabled" {
  value       = var.hosted_enabled
  description = "Whether hosted game servers are set up (var.hosted_enabled): the programs' environment then turns ONNET_HOSTED_PROVIDER=ec2 on and names the game data's store"
}

output "fleet_launch_templates" {
  value       = { for kind, template in aws_launch_template.fleet : kind => template.id }
  description = "The machines' launch templates by kind, shared and own (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES); empty while hosting is off"
}

output "hosted_subnet_ids" {
  value       = local.hosted_enabled ? [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].id] : null
  description = "The subnets machines launch into, both zones' (ONNET_HOSTED_EC2_SUBNETS); null while hosting is off"
}

output "hosted_subnet_cidrs" {
  value       = local.hosted_enabled ? [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].cidr_block] : null
  description = "Their ranges, the only peers the internal listener's machine routes answer (ONNET_HOSTED_BOOTSTRAP_CIDRS); null while hosting is off"
}

output "fleet_image_parameter" {
  value       = one(aws_ssm_parameter.fleet_image[*].name)
  description = "The SSM parameter holding the image the machines launch from (fleet bake-image writes it); null while hosting is off"
}

output "machines_security_group_id" {
  value       = one(aws_security_group.machines[*].id)
  description = "The machines' security group; null while hosting is off"
}

output "hosted_deployment" {
  value       = local.hosted_enabled ? local.hosted_deployment : null
  description = "The opennova:deployment tag of this stack's machines (ONNET_HOSTED_DEPLOYMENT); null while hosting is off"
}

output "machines_log_group" {
  value       = aws_cloudwatch_log_group.machines.name
  description = "The machines' logs"
}

output "alerts_topic_arn" {
  value       = aws_sns_topic.alerts.arn
  description = "The SNS topic every alarm notifies (its email subscription is confirmed once from the email AWS sends)"
}

output "health_check_id" {
  value       = aws_route53_health_check.service.id
  description = "The Route 53 health check on http://<the gate's address>:8080/healthz"
}
