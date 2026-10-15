# The 1Password reference for a run of the GitHub stack (infra/github, the
# organization's CI runners): the token the github provider reads from
# GITHUB_TOKEN. `op run` resolves it against the vault at run time; the value
# reaches terraform's environment alone. The toolbox fills in @VAULT@
# (OP_VAULT, default OpenNova-Deploy). The item holds a fine-grained token on
# the organization (DEPLOY.md, "CI runners"); edit the vault, not this file.
GITHUB_TOKEN=op://@VAULT@/github/token
