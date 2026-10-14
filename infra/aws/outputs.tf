output "instance_id" {
  value       = aws_instance.server.id
  description = "ID of the server EC2 instance"
}

output "public_ip" {
  value       = local.public_ip
  description = "Public IP of the server: game traffic and app deploys target it"
}

output "public_dns" {
  value       = aws_instance.server.public_dns
  description = "Public DNS name of the instance"
}

output "web_domain" {
  value       = length(cloudflare_record.web_root) > 0 ? cloudflare_record.web_root[0].hostname : null
  description = "Web root DNS record"
}

output "backup_bucket_name" {
  value       = var.backup_bucket_name
  description = "S3 bucket used for automated database backups"
}

output "region" {
  value       = var.region
  description = "The region everything here lives in (the hosted servers' ONNET_HOSTED_EC2_REGION)"
}

output "service_private_ip" {
  value       = aws_instance.server.private_ip
  description = "The server instance's private address: where hosted machines fetch their bootstrap bundle, and the address their admin.cfg admits (ONNET_HOSTED_SERVICE_ADDRESS)"
}

output "hosted_enabled" {
  value       = local.hosted_enabled
  description = "Whether hosted game servers are set up (hosted_data_snapshot_id is set): app deploy turns ONNET_HOSTED_PROVIDER=ec2 on with it"
}

output "hosted_launch_template_id" {
  value       = one(aws_launch_template.hosted[*].id)
  description = "The hosted machines' launch template (ONNET_HOSTED_EC2_LAUNCH_TEMPLATE); null while hosting is off"
}

output "hosted_subnet_id" {
  value       = local.hosted_enabled ? local.target_subnet : null
  description = "The subnet hosted machines launch into (ONNET_HOSTED_EC2_SUBNET); null while hosting is off"
}

output "hosted_subnet_cidr" {
  value       = one(data.aws_subnet.hosted[*].cidr_block)
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
