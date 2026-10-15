# 1Password references for the toolbox's own AWS calls (app, db, backup, fleet,
# secrets sync, aws): the AWS credentials alone. The toolbox resolves them once
# per command into its environment; the values never touch the disk outside the
# toolbox's tmpfs. On a CI runner with credentials of its own (the deploy
# workflow's OIDC role) the toolbox reads none of this. The toolbox fills in
# @VAULT@ (OP_VAULT, default OpenNova-Deploy).
AWS_ACCESS_KEY_ID=op://@VAULT@/aws/access_key_id
AWS_SECRET_ACCESS_KEY=op://@VAULT@/aws/secret_access_key
AWS_REGION=op://@VAULT@/aws/region
AWS_DEFAULT_REGION=op://@VAULT@/aws/region
