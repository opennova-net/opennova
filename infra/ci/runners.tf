# ---------------------------------------------------------------------------
# RunsOn Flex (runs-on.com; the module is MIT, the license key the vault's)
# ---------------------------------------------------------------------------
#
# The control plane (a small Fargate service, its queues, tables and Lambdas,
# an API Gateway ingress for GitHub's webhooks and the setup page) and the
# runners' launch templates and instance role. A job whose runs-on names
# RunsOn's labels (ci.yml) gets an instance of its own, spot unless spot is
# short (then on demand), terminated when the job ends. After the first apply
# the setup page (output setup_url) registers the GitHub App on
# github_organization; install it on the opennova repository alone.
#
# Settings past the module's defaults, each for the reason beside it.

module "runs_on" {
  source  = "runs-on/runs-on/aws//modules/flex"
  version = "v3.4.0"

  stack_name          = "opennova-ci"
  github_organization = var.github_organization
  license_key         = var.license_key
  email               = var.alert_email

  vpc_id            = aws_vpc.ci.id
  public_subnet_ids = aws_subnet.public[*].id

  # Nobody signs in to a runner: no SSH (the default) and no Session Manager.
  ssm_allowed = false
  # Each repository and branch reads and writes its own Magic Cache keys alone
  # (RunsOn's recommendation for every stack).
  enable_cache_isolation = true
  # A job hung past this is ended with its instance (the longest leg takes
  # minutes on 16 cores; RunsOn's default is 12 hours).
  runner_max_runtime = 90
  # A weekly cost email, not a daily one.
  enable_cost_reports = "weekly"
  # The account's own budget below instead: RunsOn's filters on its cost
  # allocation tag, which only the management account can turn on.
  app_budget_daily_usd = 0
}

# The account holds the runners alone, so its budget is CI's whole AWS bill.
resource "aws_budgets_budget" "monthly_cost" {
  name         = "opennova-ci-monthly-cost"
  budget_type  = "COST"
  limit_amount = tostring(var.budget_monthly_usd)
  limit_unit   = "USD"
  time_unit    = "MONTHLY"

  dynamic "notification" {
    for_each = { ACTUAL = 80, FORECASTED = 100 }
    content {
      notification_type          = notification.key
      comparison_operator        = "GREATER_THAN"
      threshold                  = notification.value
      threshold_type             = "PERCENTAGE"
      subscriber_email_addresses = [var.alert_email]
    }
  }
}

# No bucket in the account can be made public (RunsOn's cache and config
# buckets are private).
resource "aws_s3_account_public_access_block" "account" {
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}
