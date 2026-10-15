output "deployment" {
  value       = local.deployment
  description = "The deployment this state is (the workspace: default is prod)"
}

output "instance_id" {
  value       = aws_instance.service.id
  description = "The service host's instance (disposable: -replace=aws_instance.service makes a new one)"
}

output "public_ip" {
  value       = local.service_public_ip
  description = "The service's public address (its Elastic IP): retail clients' gs.novaworld.net, the gate's announced address, the site's DNS records"
}

output "service_eip_allocation_id" {
  value       = local.service_eip_allocation_id
  description = "The Elastic IP's allocation id: prod's var.service_eip_allocation_id (adopted, never managed), or the managed address of a deployment that adopts none"
}

output "service_private_ip" {
  value       = local.service_private_ip
  description = "The service host's fixed private address (its network interface's): where hosted machines reach the service, and the address their admin.cfg admits (ONNET_HOSTED_SERVICE_ADDRESS)"
}

output "web_domain" {
  value       = length(cloudflare_record.web_root) > 0 ? cloudflare_record.web_root[0].hostname : null
  description = "The site's hostname"
}

output "ops_bucket_name" {
  value       = var.ops_bucket_name
  description = "The operations bucket: releases/<deployment>/ and the database dumps (novaworld/, novaworld-<deployment>/)"
}

output "ssm_prefix" {
  value       = local.ssm_prefix
  description = "The deployment's SSM parameters: infra.env, release, secret/*"
}

output "converge_document" {
  value       = aws_ssm_document.converge.name
  description = "The SSM document that converges the host to its release (deploy/host/converge.sh)"
}

output "log_group" {
  value       = aws_cloudwatch_log_group.service.name
  description = "The containers' logs (app logs)"
}

output "db_endpoint" {
  value       = aws_db_instance.main.address
  description = "The database's hostname (private: the service host reaches it; DATABASE_URL in infra.env)"
}

output "db_port" {
  value       = aws_db_instance.main.port
  description = "The database's port"
}

output "db_identifier" {
  value       = aws_db_instance.main.identifier
  description = "The database instance (db snapshot; RDS point-in-time restores name it)"
}

output "vpc_id" {
  value       = aws_vpc.main.id
  description = "The VPC"
}

output "public_subnet_ids" {
  value       = { for k, s in aws_subnet.public : k => s.id }
  description = "The public subnets, a and b"
}

output "private_subnet_ids" {
  value       = { for k, s in aws_subnet.private : k => s.id }
  description = "The private subnets, a and b (the database's)"
}

output "region" {
  value       = var.region
  description = "The region everything here lives in (the hosted servers' ONNET_HOSTED_EC2_REGION)"
}

output "hosted_enabled" {
  value       = local.hosted_enabled
  description = "Whether hosted game servers are set up (hosted_data_snapshots names a data set): infra.env then turns ONNET_HOSTED_PROVIDER=ec2 on"
}

output "hosted_launch_templates" {
  value       = { for slug, template in aws_launch_template.hosted : slug => template.id }
  description = "The hosted machines' launch templates, one per data set, by the data set's games slug (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES in infra.env); empty while hosting is off"
}

output "hosted_subnet_id" {
  value       = local.hosted_enabled ? aws_subnet.public["a"].id : null
  description = "The subnet hosted machines launch into (ONNET_HOSTED_EC2_SUBNET); null while hosting is off"
}

output "hosted_subnet_cidr" {
  value       = local.hosted_enabled ? aws_subnet.public["a"].cidr_block : null
  description = "That subnet's range, the only peers the bootstrap route answers (ONNET_HOSTED_BOOTSTRAP_CIDRS); null while hosting is off"
}

output "hosted_security_group_id" {
  value       = one(aws_security_group.hosted[*].id)
  description = "The hosted machines' security group; null while hosting is off"
}

output "hosted_deployment" {
  value       = local.hosted_enabled ? local.hosted_deployment : null
  description = "The opennova:deployment tag of this stack's hosted machines (ONNET_HOSTED_DEPLOYMENT); null while hosting is off"
}
