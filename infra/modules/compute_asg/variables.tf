variable "name_prefix" { type = string }
variable "vpc_id" { type = string }
variable "subnet_id" { type = string }
variable "security_group_id" { type = string }
variable "instance_profile_arn" { type = string }

variable "instance_type" { type = string }
variable "ebs_volume_gb" {
  type = number

  validation {
    condition     = var.ebs_volume_gb >= 30
    error_message = "ebs_volume_gb must be at least 30 to satisfy the selected AMI snapshot size."
  }
}

variable "asg_min" { type = number }
variable "asg_desired" { type = number }
variable "asg_max" { type = number }

variable "allocate_eip" { type = bool }

variable "app_git_repo" { type = string }
variable "app_git_ref" { type = string }
variable "app_build_target" { type = string }
variable "app_listen_port" { type = number }
variable "app_env" { type = map(string) }
variable "allow_ssh" { type = bool }
variable "ssh_key_name" {
  type     = string
  default  = null
  nullable = true
}

variable "cloudwatch_log_group_name" { type = string }

variable "tags" { type = map(string) }

variable "domain_name" { type = string }
variable "letsencrypt_email" { type = string }
