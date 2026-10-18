# Deploy toolbox: everything an operator needs to stand up and update an
# OpenNova NovaWorld deployment, bundled so the operator's machine needs only
# docker. Debian (glibc), so the AWS CLI v2 and the Session Manager plugin run
# natively: the docker CLI with its compose and buildx plugins (buildx
# resolves image tags to digests), the 1Password CLI (op), terraform, packer
# (`ci image`), the AWS CLI v2, the Session Manager plugin (`app shell`), jq
# and git. Every download
# is a pinned release checked against its SHA-256 before it is unpacked; a bump
# moves a version and its sum together. x86_64 only (docker-compose.deploy.yml
# pins the platform; an arm64 machine runs it emulated).
#
# The ONLY secret the operator supplies is OP_SERVICE_ACCOUNT_TOKEN; every
# other secret (AWS keys, the Cloudflare token, the app's secrets) is read
# from the 1Password vault at deploy time.

FROM debian:bookworm-slim

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# 1Password CLI (https://app-updates.agilebits.com/product_history/CLI2).
ARG OP_VERSION=2.30.3
ARG OP_SHA256=a16307ebcecb40fd091d7a6ff4f0c380c3c0897c4f4616de2c5d285e57d5ee28
# Terraform (https://releases.hashicorp.com/terraform/<v>/terraform_<v>_SHA256SUMS).
# .github/workflows/infra.yml tests with the same version: bump both together.
ARG TERRAFORM_VERSION=1.11.4
ARG TERRAFORM_SHA256=1ce994251c00281d6845f0f268637ba50c0005657eb3cf096b92f753b42ef4dc
# AWS CLI v2: the release apps/novaworld_common/Dockerfile pins, its sum taken
# from the release AWS signed.
ARG AWSCLI_VERSION=2.36.50
ARG AWSCLI_SHA256=0f15318b3edc4133ad71ce550303babdf2b5dc66bee6b91b4ac8bf05407a78bf
# Packer (https://releases.hashicorp.com/packer/<v>/packer_<v>_SHA256SUMS): the
# CI runners' Windows image (infra/ci/image, `ci image`).
ARG PACKER_VERSION=1.16.1
ARG PACKER_SHA256=af38a9e93e4ed1b9ca68206ae969c64c300c82a3dde46a780dfa629f0867f651
# Session Manager plugin (https://docs.aws.amazon.com/systems-manager/latest/userguide/plugin-version-history.html).
ARG SSM_PLUGIN_VERSION=1.2.835.0
ARG SSM_PLUGIN_SHA256=7c6dcad12518571cc7959a713e6a8ae1bdf6ed66fd9bee37dc189e39ca58ae03
# Docker's apt signing key.
ARG DOCKER_REPO_FINGERPRINT=9DC858229FC7DD38854AE2D88D81803C0EBFCD88

RUN [ "$(dpkg --print-architecture)" = amd64 ] \
    || { echo "the toolbox is x86_64 only (docker-compose.deploy.yml pins linux/amd64)"; exit 1; }

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        bash ca-certificates curl git gnupg jq less openssl unzip \
    && rm -rf /var/lib/apt/lists/*

# The docker CLI, compose and buildx, from Docker's apt repository.
RUN install -m 0755 -d /etc/apt/keyrings \
    && curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc \
    && fpr="$(gpg --show-keys --with-colons /etc/apt/keyrings/docker.asc | awk -F: '$1 == "fpr" { print $10; exit }')" \
    && { [ "$fpr" = "$DOCKER_REPO_FINGERPRINT" ] || { echo "Docker's apt key has fingerprint $fpr"; exit 1; }; } \
    && echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/debian bookworm stable" \
        > /etc/apt/sources.list.d/docker.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends docker-ce-cli docker-compose-plugin docker-buildx-plugin \
    && rm -rf /var/lib/apt/lists/* \
    && docker --version && docker compose version && docker buildx version

RUN curl -fsSL "https://cache.agilebits.com/dist/1P/op2/pkg/v${OP_VERSION}/op_linux_amd64_v${OP_VERSION}.zip" -o /tmp/op.zip \
    && echo "${OP_SHA256}  /tmp/op.zip" | sha256sum -c - \
    && unzip -q -o /tmp/op.zip op -d /usr/local/bin/ \
    && rm /tmp/op.zip \
    && op --version

RUN curl -fsSL "https://releases.hashicorp.com/terraform/${TERRAFORM_VERSION}/terraform_${TERRAFORM_VERSION}_linux_amd64.zip" -o /tmp/tf.zip \
    && echo "${TERRAFORM_SHA256}  /tmp/tf.zip" | sha256sum -c - \
    && unzip -q -o /tmp/tf.zip terraform -d /usr/local/bin/ \
    && rm /tmp/tf.zip \
    && terraform version

RUN curl -fsSL "https://releases.hashicorp.com/packer/${PACKER_VERSION}/packer_${PACKER_VERSION}_linux_amd64.zip" -o /tmp/packer.zip \
    && echo "${PACKER_SHA256}  /tmp/packer.zip" | sha256sum -c - \
    && unzip -q -o /tmp/packer.zip packer -d /usr/local/bin/ \
    && rm /tmp/packer.zip \
    && packer version

RUN curl -fsSL "https://awscli.amazonaws.com/awscli-exe-linux-x86_64-${AWSCLI_VERSION}.zip" -o /tmp/awscliv2.zip \
    && echo "${AWSCLI_SHA256}  /tmp/awscliv2.zip" | sha256sum -c - \
    && unzip -q /tmp/awscliv2.zip -d /tmp \
    && /tmp/aws/install --install-dir /usr/local/aws-cli --bin-dir /usr/local/bin \
    && rm -rf /tmp/aws /tmp/awscliv2.zip \
    && aws --version

RUN curl -fsSL "https://s3.amazonaws.com/session-manager-downloads/plugin/${SSM_PLUGIN_VERSION}/ubuntu_64bit/session-manager-plugin.deb" -o /tmp/smp.deb \
    && echo "${SSM_PLUGIN_SHA256}  /tmp/smp.deb" | sha256sum -c - \
    && dpkg -i /tmp/smp.deb \
    && rm /tmp/smp.deb \
    && session-manager-plugin --version

# The repo is mounted at /workspace; git reads its revision for a release's
# manifest (the mount's owner is not the container's user).
RUN git config --system --add safe.directory /workspace

WORKDIR /workspace
ENTRYPOINT ["/workspace/deploy/bin/on-deploy"]
CMD ["help"]
