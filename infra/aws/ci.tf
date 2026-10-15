# ---------------------------------------------------------------------------
# The optional GitHub Actions deploy (.github/workflows/deploy.yml; DEPLOY.md,
# section 4)
# ---------------------------------------------------------------------------
#
# With var.github_deploy on (prod alone), GitHub's OIDC provider and a role the
# deploy workflow assumes for one run: trusted only by this repository's jobs
# in its `production` environment (whose required reviewers are the approval
# gate), and able to do what `app deploy` and `app rollback` do and no more:
# read the deployment's infra.env, upload and read its release bundles, point
# its release parameter, find its service instance, and run its converge document
# there. No vault, no Terraform, no secrets: the host reads those itself.

locals {
  github_deploy = local.manage_shared && var.github_deploy
}

resource "aws_iam_openid_connect_provider" "github" {
  count          = local.github_deploy ? 1 : 0
  url            = "https://token.actions.githubusercontent.com"
  client_id_list = ["sts.amazonaws.com"]
  # GitHub's published thumbprints (AWS verifies GitHub's tokens against its own
  # trust store as well).
  thumbprint_list = ["6938fd4d98bab03faadb97b34396831e3780aea1", "1c58a3a8518e8759bf075b76b750d4f2df264fcd"]

  tags = var.tags
}

data "aws_iam_policy_document" "github_deploy_trust" {
  count = local.github_deploy ? 1 : 0

  statement {
    effect  = "Allow"
    actions = ["sts:AssumeRoleWithWebIdentity"]

    principals {
      type        = "Federated"
      identifiers = [aws_iam_openid_connect_provider.github[0].arn]
    }

    condition {
      test     = "StringEquals"
      variable = "token.actions.githubusercontent.com:aud"
      values   = ["sts.amazonaws.com"]
    }
    condition {
      test     = "StringEquals"
      variable = "token.actions.githubusercontent.com:sub"
      values   = ["repo:${var.github_repository}:environment:${var.github_environment}"]
    }
  }
}

resource "aws_iam_role" "github_deploy" {
  count                = local.github_deploy ? 1 : 0
  name                 = "${local.name}-github-deploy"
  description          = "The approval-gated GitHub Actions deploy of the ${local.deployment} NovaWorld stack (.github/workflows/deploy.yml)"
  assume_role_policy   = data.aws_iam_policy_document.github_deploy_trust[0].json
  max_session_duration = 3600

  tags = var.tags
}

data "aws_iam_policy_document" "github_deploy" {
  count = local.github_deploy ? 1 : 0

  statement {
    sid       = "ReadInfraEnv"
    effect    = "Allow"
    actions   = ["ssm:GetParameter"]
    resources = [aws_ssm_parameter.infra_env.arn, aws_ssm_parameter.release.arn]
  }

  statement {
    sid       = "PointRelease"
    effect    = "Allow"
    actions   = ["ssm:PutParameter"]
    resources = [aws_ssm_parameter.release.arn]
  }

  statement {
    sid       = "ListReleases"
    effect    = "Allow"
    actions   = ["s3:ListBucket"]
    resources = [local.ops_bucket_arn]

    condition {
      test     = "StringLike"
      variable = "s3:prefix"
      values   = ["releases/${local.deployment}/*"]
    }
  }

  statement {
    sid       = "WriteReleases"
    effect    = "Allow"
    actions   = ["s3:PutObject", "s3:GetObject"]
    resources = ["${local.ops_bucket_arn}/releases/${local.deployment}/*"]
  }

  statement {
    sid       = "FindServiceHost"
    effect    = "Allow"
    actions   = ["ec2:DescribeInstances"]
    resources = ["*"]
  }

  # The converge document, on the instance tagged as this deployment's service
  # host alone.
  statement {
    sid       = "SendConvergeDocument"
    effect    = "Allow"
    actions   = ["ssm:SendCommand"]
    resources = [aws_ssm_document.converge.arn]
  }

  statement {
    sid       = "SendToServiceHost"
    effect    = "Allow"
    actions   = ["ssm:SendCommand"]
    resources = ["arn:${local.partition}:ec2:${var.region}:${local.account_id}:instance/*"]

    condition {
      test     = "StringEquals"
      variable = "ssm:resourceTag/opennova:service"
      values   = [local.deployment]
    }
  }

  statement {
    sid       = "ReadCommandResults"
    effect    = "Allow"
    actions   = ["ssm:GetCommandInvocation", "ssm:ListCommandInvocations"]
    resources = ["*"]
  }
}

resource "aws_iam_role_policy" "github_deploy" {
  count  = local.github_deploy ? 1 : 0
  name   = "${local.name}-github-deploy"
  role   = aws_iam_role.github_deploy[0].id
  policy = data.aws_iam_policy_document.github_deploy[0].json
}
