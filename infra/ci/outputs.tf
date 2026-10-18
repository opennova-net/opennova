# The setup page: after the first apply, open it to register the GitHub App
# (DEPLOY.md, "CI runners"). It is also the address GitHub's webhooks reach.
output "setup_url" {
  value       = module.runs_on.ingress.url
  description = "RunsOn's setup page and webhook ingress"
}
