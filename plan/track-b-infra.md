# Track B: infrastructure, deployment, 1Password secrets

## Constraints

1. Secrets come only from a 1Password vault. The operator handles exactly one secret:
   a service-account token (`OP_SERVICE_ACCOUNT_TOKEN`). No ansible-vault, no committed
   env files with real values, no vault password files.
2. Every machine involved needs only docker. The deploy tooling (op CLI, terraform,
   docker CLI, ssh) runs in a toolbox container; the target EC2 host runs containers
   over `DOCKER_HOST=ssh://` and never needs python, rsync, or anything beyond docker
   and the AMI's sshd.
3. Anyone can deploy their own instance: their own vault, AWS account, Cloudflare zone,
   same commands. `DEPLOY.md` is the third-party guide.

## Topology

One C++ server container (gate UDP 7597, NovaWorld UDP 64206, HTTP 8080) + one web
container (nginx serving the built portal, proxying `/api/`) + a backup sidecar. SQLite
in a named volume (`nw_data`), no database container.

```
deploy/
  Dockerfile                   # toolbox: op, terraform, docker cli + compose, ssh, jq
  docker-compose.deploy.yml    # repo at /workspace, tmpfs /run/secrets, OP token env
  run.sh / run.ps1             # ./deploy/run.sh <cmd...>
  bin/on-deploy                # secrets|infra|app|backup|state subcommands
  env/
    terraform.env.tpl          # op:// references (AWS, Cloudflare, ssh pubkey)
    app.prod.env.tpl           # op:// references (NWU keys, admin auth)
    app.prod.env               # committed non-secret prod values
    app.dev.env                # committed dev values, dev keys included
  compose/
    docker-compose.yml         # base: server + web
    docker-compose.dev.yml     # published ports, bind mounts, vite dev
    docker-compose.prod.yml    # GHCR images, host networking, restart, backup sidecar
  backup/                      # cron container: sqlite3 .backup -> S3, restore.sh
infra/aws/                     # terraform (VPC, SG, EC2, EIP, Cloudflare, S3, CloudFront)
apps/novaworld_server/Dockerfile
DEPLOY.md
```

## Vault schema (`OpenNova-Deploy`)

| Item | Fields |
|------|--------|
| `aws` | access_key_id, secret_access_key, region |
| `cloudflare` | api_token, zone_id, domain |
| `ssh` | 1Password-generated key pair |
| `app-prod` | admin_api_token, admin_basic_auth_user, admin_basic_auth_password |
| `tfstate` | terraform state as a Document (`state push`/`state pull`) |

## Operator UX

```
export OP_SERVICE_ACCOUNT_TOKEN=ops_...
./deploy/run.sh secrets check
./deploy/run.sh infra plan|apply
./deploy/run.sh app deploy [--tag <sha>|--build]
./deploy/run.sh app status|logs|restart|down
./deploy/run.sh backup now|list
```

`app deploy`: pull state, read the instance IP from terraform output, `op read` the SSH
key into tmpfs, `op inject` the app env, then drive
`DOCKER_HOST=ssh://ubuntu@<ip> docker compose ... pull && up -d`.

## Terraform deltas vs the reference

- Key pair created from the vault-sourced public key.
- New unproxied A record `nw.<domain>` (TTL 300): the launcher's resolution anchor.
  `@`/`www` stay Cloudflare-proxied for web TLS; game traffic is unproxied.
- IMDSv2 required; backup bucket lifecycle (90 days); user_data installs docker +
  compose plugin only.
- Keep: downloads bucket + ACM + CloudFront (`downloads.<domain>`), launcher-ci IAM
  user whose keys go to GitHub secrets for the launcher publish flow.

## CI/CD

- `ci.yml`: integration branch added to PR triggers (PR 1). Win+mac matrix unchanged.
- `novaworld-images.yml`: path-filtered; ubuntu job with `BUILD_NOVAWORLD_HTTP=ON` and
  scoped ctest, then server + web image build-push to GHCR (`sha-<short>` + branch
  tags, `latest` on master). Images public so deployers pull without building.
- `launcher-publish.yml`: tag `launcher-v*`; publish to GH Release always; S3
  `downloads.<domain>/launcher/` + `version.json`/`app.json` when AWS secrets are
  present.
- No deploy-from-CI: production deploys run from an operator machine so GitHub never
  holds prod credentials.

## Cutover

Fresh EC2 + EIP from this repo. Staging smoke first (PR 21): deploy from a docker-only
machine, retail JO over the internet, backup verified, destroy. Then prod apply, fresh
database (seed + new registrations; no migration from the old postgres), Cloudflare
records flip, the launcher's server-info returns the new IP, the opennova-int box
retires.
