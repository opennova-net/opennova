variable "deployment" {
  description = "The deployment this account holds: prod or staging."
  type        = string

  validation {
    condition     = contains(["prod", "staging"], var.deployment)
    error_message = "deployment is prod or staging."
  }
}

variable "account_id" {
  description = "The account's id (the policies name it, the state bucket's name carries it)."
  type        = string
}

variable "region" {
  description = "The account's region."
  type        = string
}

variable "github_repository" {
  description = "The repository (owner/name) whose deploy workflow, in its environment of the deployment's name alone, may assume the GitHub deploy role."
  type        = string
}

variable "state_stacks" {
  description = "The stacks whose state the deploy user reads and writes in this account's bucket (infra/<stack>/terraform.tfstate): aws everywhere; prod also the shared zone and github."
  type        = list(string)

  validation {
    condition     = alltrue([for s in var.state_stacks : contains(["aws", "zone", "github"], s)])
    error_message = "state_stacks takes aws, zone and github."
  }
}
