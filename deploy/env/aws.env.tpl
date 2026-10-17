# 1Password references for the toolbox's AWS identity: the access key of the
# deployment's own deploy user (the user opennova-deploy of the deployment's
# account, which the org stack makes and writes into the deployment's vault
# item aws-<deployment>), or for `org` the management account's opennova-org
# user (item aws-org). A command reads the one item its deployment names; the
# other deployment's only for the two one-way reads that say so by their flag
# (`backup import --from-deployment prod`, `app deploy --promote staging`),
# each in a subshell of its own. The toolbox resolves them from its run's
# cache of the vault (each item read once per run; on-deploy, "The vault");
# the values never touch the disk outside the toolbox's tmpfs. On a
# CI runner with credentials of its own (the deploy workflow's OIDC role) the
# toolbox reads none of this. The toolbox fills in @VAULT@ (OP_VAULT, default
# OpenNova-Deploy) and @AWS_ITEM@.
AWS_ACCESS_KEY_ID=op://@VAULT@/@AWS_ITEM@/access_key_id
AWS_SECRET_ACCESS_KEY=op://@VAULT@/@AWS_ITEM@/secret_access_key
