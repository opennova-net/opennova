# Prod's admin reads CI's whole account, including runners, cache and control
# plane. This is the only runtime cross-account access: billing reads alone.
# Trust the exact role ARN as a condition so org can run before infra/aws has
# created prod's web role, and replacing that role preserves the trust.
locals {
  ci_cost_role_arn = "arn:aws:iam::${var.aws_account_id_ci}:role/opennova-ci-cost-read"
}

resource "aws_iam_role" "ci_cost_read" {
  provider = aws.ci
  name     = "opennova-ci-cost-read"
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Action    = "sts:AssumeRole"
      Principal = { AWS = "arn:aws:iam::${var.aws_account_id_prod}:root" }
      Condition = { ArnEquals = { "aws:PrincipalArn" = "arn:aws:iam::${var.aws_account_id_prod}:role/opennova-prod-web" } }
    }]
  })
}

resource "aws_iam_role_policy" "ci_cost_read" {
  provider = aws.ci
  name     = "opennova-ci-cost-read"
  role     = aws_iam_role.ci_cost_read.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      { Effect = "Allow", Action = ["ce:GetCostAndUsage", "ce:GetCostForecast"], Resource = "*" },
      { Effect = "Allow", Action = "budgets:ViewBudget", Resource = "arn:aws:budgets::${var.aws_account_id_ci}:budget/opennova-ci-monthly-cost" },
    ]
  })
}
