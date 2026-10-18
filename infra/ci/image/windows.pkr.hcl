# The CI runners' Windows image (DEPLOY.md, "CI runners"): RunsOn's newest
# windows25-full-x64 (Windows Server 2025 with Visual Studio 2022) with Visual
# Studio 2026's C++ build tools added, the toolchain GitHub's windows-latest
# and the maintainers build with (VS 2022's MSVC 14.44 compiles and runs some
# of the tree differently). vswhere -latest, and so ilammy/msvc-dev-cmd, finds
# 2026. The AMI stays in the ci account, private; ci.yml's Windows legs name
# it (image=ami-...).
#
# `./deploy/run.sh ci image` builds it (about an hour), as opennova-org
# through the ci account's OrganizationAccountAccessRole: the builder runs in
# the runners' public subnets, WinRM over HTTPS reachable from the operator's
# address alone, and ends as RunsOn's own build does (WinRM off, EC2Launch
# reset, so an instance of the image runs its user data at boot), with the
# build's Administrator password replaced by one nobody holds.

packer {
  required_plugins {
    amazon = {
      source  = "github.com/hashicorp/amazon"
      version = "= 1.8.2"
    }
  }
}

variable "account_id" {
  type        = string
  description = "The ci account (the vault's aws-ci/account_id)."
}

variable "region" {
  type        = string
  description = "The ci account's region (aws-ci/region)."
}

variable "winrm_password" {
  type        = string
  sensitive   = true
  description = "The Administrator password for the build alone (the toolbox makes one per build)."
}

locals {
  stamp = formatdate("YYYYMMDDhhmmss", timestamp())
  tags = {
    "opennova:deployment" = "opennova-ci"
  }
}

source "amazon-ebs" "windows" {
  region = var.region

  assume_role {
    role_arn     = "arn:aws:iam::${var.account_id}:role/OrganizationAccountAccessRole"
    session_name = "opennova-ci-image"
  }

  # RunsOn's published images (its AWS account; runs-on/runner-images-for-aws).
  source_ami_filter {
    filters = {
      name                = "runs-on-v2.2-windows25-full-x64-*"
      virtualization-type = "hvm"
      root-device-type    = "ebs"
    }
    owners      = ["135269210855"]
    most_recent = true
  }

  instance_type = "c7i.2xlarge"

  subnet_filter {
    filters = {
      "tag:Name" = "opennova-ci-public-*"
    }
    random = true
  }
  associate_public_ip_address               = true
  temporary_security_group_source_public_ip = true

  # ci.yml's Windows legs ask for 150 GB (volume=150gb); the image may not be larger.
  launch_block_device_mappings {
    device_name           = "/dev/sda1"
    volume_type           = "gp3"
    volume_size           = 150
    delete_on_termination = true
  }

  ami_name        = "opennova-ci-windows25-vs2026-${local.stamp}"
  ami_description = "OpenNova CI: RunsOn windows25-full-x64 with Visual Studio 2026 Build Tools (C++)"
  tags            = local.tags
  run_tags        = local.tags
  snapshot_tags   = local.tags

  communicator   = "winrm"
  winrm_username = "Administrator"
  winrm_password = var.winrm_password
  winrm_use_ssl  = true
  winrm_insecure = true
  winrm_timeout  = "30m"

  # RunsOn's image leaves WinRM disabled; the builder turns it on over HTTPS.
  user_data = templatefile("${path.root}/user_data.ps1.tpl", { password = var.winrm_password })
}

build {
  sources = ["source.amazon-ebs.windows"]

  provisioner "powershell" {
    script = "${path.root}/install-vs2026.ps1"
  }

  provisioner "windows-restart" {
    restart_timeout = "30m"
  }

  # Last, as RunsOn's build ends: a moment after this step returns (Packer
  # then stops the instance through the EC2 API), the Administrator password
  # becomes a random one and WinRM stops; WinRM stays disabled in the image,
  # and EC2Launch's reset runs user data at the next boot.
  provisioner "powershell" {
    skip_clean = true
    inline = [
      "Set-Service -Name WinRM -StartupType Disabled",
      "$null = Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', 'Start-Sleep -Seconds 15; $b = New-Object byte[] 24; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); net user Administrator ([Convert]::ToBase64String($b) + ''aA1!'') | Out-Null; Stop-Service -Name WinRM -Force -ErrorAction SilentlyContinue')",
      "& \"C:\\Program Files\\Amazon\\EC2Launch\\EC2Launch.exe\" reset",
      "exit 0",
    ]
  }
}
