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
