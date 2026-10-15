output "windows_runner" {
  description = "The Windows runner's name: ci.yml's runs-on label for the Windows build legs."
  value       = github_actions_hosted_runner.windows.name
}

output "linux_runner" {
  description = "The Linux runner's name: ci.yml's runs-on label for the Linux build legs."
  value       = github_actions_hosted_runner.linux.name
}
