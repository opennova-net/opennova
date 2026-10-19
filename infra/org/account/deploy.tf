# ---------------------------------------------------------------------------
# The deploy user, its policies and the two boundaries (infra/aws/iam/README.md)
# ---------------------------------------------------------------------------
#
# opennova-deploy is what the toolbox and every infra/aws run act as in this
# account: an IAM user whose key the org stack makes and the toolbox writes
# into the deployment's vault item (never printed). Its permissions are the
# six policies under infra/aws/iam/, OpenNova's names and tags alone, and two
# inline ones: the Terraform states it runs, and a guard that keeps it off the
# state bucket's settings and old versions. Every role infra/aws makes carries
# a boundary, which the deploy user can neither change nor take off: the
# machine boundary on the machines' two roles (the instance role, hostd's task
# role), the role boundary on every other; no role it writes holds more than
# its boundary allows.
#
# A policy file is a template (account_id, region); IAM counts a managed
# policy's characters without white space, at most 6,144 (tests/ check each).

locals {
  policy_dir      = "${path.module}/../../aws/iam"
  deploy_policies = toset(["network", "compute", "ecs", "iam", "data", "observability"])
  policy_vars     = { account_id = var.account_id, region = var.region }

  deploy_policy_documents = {
    for p in local.deploy_policies : p => jsonencode(jsondecode(templatefile("${local.policy_dir}/opennova-deploy-${p}.json", local.policy_vars)))
  }
  boundary_document         = jsonencode(jsondecode(templatefile("${local.policy_dir}/opennova-role-boundary.json", local.policy_vars)))
  machine_boundary_document = jsonencode(jsondecode(templatefile("${local.policy_dir}/opennova-machine-boundary.json", local.policy_vars)))
}

# Its description is the one it was made with: IAM never changes a policy's
# description (a new one would replace the policy, which every role carries).
resource "aws_iam_policy" "boundary" {
  name        = "opennova-role-boundary"
  description = "The most any role infra/aws makes may do (infra/aws/iam/opennova-role-boundary.json)"
  policy      = local.boundary_document
}

resource "aws_iam_policy" "machine_boundary" {
  name        = "opennova-machine-boundary"
  description = "The most the machines' instance role and hostd's task role may do (infra/aws/iam/opennova-machine-boundary.json)"
  policy      = local.machine_boundary_document
}

resource "aws_iam_policy" "deploy" {
  for_each    = local.deploy_policies
  name        = "opennova-deploy-${each.key}"
  description = "OpenNova's deploy user (infra/aws/iam/opennova-deploy-${each.key}.json)"
  policy      = local.deploy_policy_documents[each.key]
}

resource "aws_iam_user" "deploy" {
  name = local.deploy_user_name
}

resource "aws_iam_user_policy_attachment" "deploy" {
  for_each   = local.deploy_policies
  user       = aws_iam_user.deploy.name
  policy_arn = aws_iam_policy.deploy[each.key].arn
}

# The toolbox's key. Its secret is in this stack's state (the org stack's,
# in prod's encrypted, private state bucket) and goes from its output straight
# into the vault item aws-<deployment>. Rotating it: a -replace of this, an
# apply, the vault written again.
resource "aws_iam_access_key" "deploy" {
  user = aws_iam_user.deploy.name
}

# The states the deploy user runs: read and write them, take and drop the S3
# backend's lock files. Never a version's deletion (an old state stays
# recoverable), and never the org stack's state (its administrator role's).
resource "aws_iam_user_policy" "state" {
  name = "opennova-deploy-state"
  user = aws_iam_user.deploy.name
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Sid      = "StateBucket"
        Effect   = "Allow"
        Action   = ["s3:ListBucket", "s3:GetBucketVersioning", "s3:GetBucketLocation"]
        Resource = aws_s3_bucket.state.arn
      },
      {
        Sid      = "StateObjects"
        Effect   = "Allow"
        Action   = ["s3:GetObject", "s3:PutObject"]
        Resource = [for s in var.state_stacks : "${aws_s3_bucket.state.arn}/infra/${s}/*"]
      },
      {
        Sid      = "StateLocks"
        Effect   = "Allow"
        Action   = "s3:DeleteObject"
        Resource = [for s in var.state_stacks : "${aws_s3_bucket.state.arn}/infra/${s}/terraform.tfstate.tflock"]
      },
    ]
  })
}

# What the org stack alone may change: the state bucket's settings and its old
# versions (opennova-deploy-data allows bucket settings on opennova-* names,
# which the state bucket shares).
resource "aws_iam_user_policy" "guard" {
  name = "opennova-deploy-guard"
  user = aws_iam_user.deploy.name
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Sid    = "NotTheStateBucketsSettings"
        Effect = "Deny"
        Action = [
          "s3:DeleteBucket", "s3:DeleteBucketPolicy", "s3:DeleteObjectVersion", "s3:PutBucketPolicy",
          "s3:PutBucketPublicAccessBlock", "s3:PutBucketVersioning", "s3:PutEncryptionConfiguration",
          "s3:PutLifecycleConfiguration",
        ]
        Resource = [aws_s3_bucket.state.arn, "${aws_s3_bucket.state.arn}/*"]
      },
    ]
  })
}
