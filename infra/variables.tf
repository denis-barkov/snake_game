variable "aws_region" {
  type    = string
  default = "us-east-1"
}

variable "az" {
  type    = string
  default = "us-east-1a"
}

variable "project" {
  type = string
}

variable "environment" {
  type    = string
  default = "mvp"
}

variable "owner" {
  type = string
}

variable "instance_type" {
  type    = string
  default = "t4g.small"
}

variable "app_control_port" {
  type    = number
  default = 8080
}

# If you truly want SSH: allow_ssh=true and set your public IP CIDR.
variable "allow_ssh" {
  type    = bool
  default = false
}

variable "admin_ip_cidr" {
  type    = string
  default = "0.0.0.0/32"
}

variable "ssh_key_name" {
  type        = string
  default     = null
  nullable    = true
  description = "Optional EC2 key pair name for SSH. If null and exactly one key pair exists, Terraform auto-selects it."
}

variable "domain_name" {
  type        = string
  description = "Public domain pointing to the instance EIP (A record). Required for Let's Encrypt."
}

variable "route53_zone_name" {
  type        = string
  description = "Existing public Route53 hosted zone name to use (do not create/modify the zone)."
  default     = "terrariumsnake.com"
}

variable "letsencrypt_email" {
  type        = string
  description = "Email for Let's Encrypt registration."
}

# App deployment (MVP pulls code and builds)
variable "app_git_repo" {
  type        = string
  description = "Git repo URL (https) containing your C++ server"
}

variable "app_git_ref" {
  type        = string
  description = "Git ref/branch/tag to checkout"
  default     = "main"
}

variable "app_build_target" {
  type        = string
  description = "Relative path to the C++ source to compile (e.g., api/snake_server_ws.cpp)"
}

# App env variables exported into systemd service
variable "app_env" {
  type = map(string)
  default = {
    SNAKE_W            = "40"
    SNAKE_H            = "20"
    SNAKE_MAX_PER_USER = "3"
    TICK_HZ            = "10"
    SPECTATOR_HZ       = "10"
    ENABLE_BROADCAST   = "true"
    LOG_HZ             = "true"
  }
}
