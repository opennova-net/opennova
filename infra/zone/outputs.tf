output "deployment_hosts" {
  value       = local.deployment_hosts
  description = "Each deployment's web hostnames, which its edge-authentication rule stamps"
}

output "email_routing" {
  value       = local.email_enabled ? [for l in var.email_routes : "${l}@${var.cloudflare_domain}"] : []
  description = "The addresses Email Routing forwards (none while the destination is a placeholder)"
}
