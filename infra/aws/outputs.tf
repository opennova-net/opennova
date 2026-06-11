output "instance_id" {
  value       = aws_instance.server.id
  description = "ID of the server EC2 instance"
}

output "public_ip" {
  value       = local.public_ip
  description = "Public IP the launcher's hosts entries and game traffic target"
}

output "public_dns" {
  value       = aws_instance.server.public_dns
  description = "Public DNS name of the instance"
}

output "web_domain" {
  value       = length(cloudflare_record.web_root) > 0 ? cloudflare_record.web_root[0].hostname : null
  description = "Web root DNS record"
}

output "nw_domain" {
  value       = length(cloudflare_record.nw) > 0 ? cloudflare_record.nw[0].hostname : null
  description = "Unproxied NovaWorld anchor record (what the launcher resolves)"
}

output "downloads_bucket_name" {
  value       = length(aws_s3_bucket.downloads) > 0 ? aws_s3_bucket.downloads[0].bucket : null
  description = "Public S3 bucket hosting launcher downloads"
}

output "backup_bucket_name" {
  value       = var.backup_bucket_name
  description = "S3 bucket used for automated database backups"
}

output "cloudflare_downloads_domain" {
  value       = length(cloudflare_record.downloads) > 0 ? cloudflare_record.downloads[0].hostname : null
  description = "Downloads subdomain CNAME"
}

output "ci_user_access_key_id" {
  value       = length(aws_iam_access_key.launcher_ci) > 0 ? aws_iam_access_key.launcher_ci[0].id : null
  description = "Access key ID for the GitHub Actions launcher upload user (copy to GH secrets via `gh secret set`)"
  sensitive   = true
}

output "ci_user_secret_access_key" {
  value       = length(aws_iam_access_key.launcher_ci) > 0 ? aws_iam_access_key.launcher_ci[0].secret : null
  description = "Secret access key for the GitHub Actions launcher upload user"
  sensitive   = true
}

output "cloudfront_downloads_domain" {
  value       = length(aws_cloudfront_distribution.downloads) > 0 ? aws_cloudfront_distribution.downloads[0].domain_name : null
  description = "CloudFront domain serving downloads"
}
