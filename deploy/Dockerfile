# Deploy toolbox: everything an operator needs to stand up and update an
# OpenNova NovaWorld instance, bundled so the operator machine needs only
# docker. The docker:cli base ships the docker client + compose plugin
# (used over DOCKER_HOST=ssh:// to drive the remote host); we add the
# 1Password CLI (op), terraform, openssh, jq, curl, and bash.
#
# The ONLY secret the operator supplies is OP_SERVICE_ACCOUNT_TOKEN; every
# other secret (AWS keys, Cloudflare token, SSH key, app env) is read from
# the 1Password vault at deploy time.

FROM docker:27-cli

ARG OP_VERSION=2.30.3
ARG TERRAFORM_VERSION=1.9.5

RUN apk add --no-cache \
        bash ca-certificates curl unzip jq openssh-client git

# Note: the AWS CLI is NOT installed in this image. Python-based aws-cli does
# not run on musl/Alpine (v1 hits a pyexpat/libexpat ABI skew; v2's bundled
# PyInstaller needs glibc's dladdr1, absent under gcompat). Instead, the `aws`
# subcommand in on-deploy runs the official glibc `amazon/aws-cli` image via the
# Docker socket (mounted in docker-compose.deploy.yml) — always works, no
# musl-python pain.

# 1Password CLI
RUN set -eux; \
    case "$(apk --print-arch)" in \
      x86_64) op_arch=amd64 ;; \
      aarch64) op_arch=arm64 ;; \
      *) echo "unsupported arch"; exit 1 ;; \
    esac; \
    curl -fsSL "https://cache.agilebits.com/dist/1P/op2/pkg/v${OP_VERSION}/op_linux_${op_arch}_v${OP_VERSION}.zip" -o /tmp/op.zip; \
    unzip -o /tmp/op.zip op -d /usr/local/bin/; \
    rm /tmp/op.zip; \
    op --version

# Terraform
RUN set -eux; \
    case "$(apk --print-arch)" in \
      x86_64) tf_arch=amd64 ;; \
      aarch64) tf_arch=arm64 ;; \
      *) echo "unsupported arch"; exit 1 ;; \
    esac; \
    curl -fsSL "https://releases.hashicorp.com/terraform/${TERRAFORM_VERSION}/terraform_${TERRAFORM_VERSION}_linux_${tf_arch}.zip" -o /tmp/tf.zip; \
    unzip -o /tmp/tf.zip terraform -d /usr/local/bin/; \
    rm /tmp/tf.zip; \
    terraform version

WORKDIR /workspace
ENTRYPOINT ["/workspace/deploy/bin/on-deploy"]
CMD ["help"]
