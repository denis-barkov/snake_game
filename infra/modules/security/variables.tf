variable "vpc_id" { type = string }
variable "name_prefix" { type = string }
variable "allow_ssh" { type = bool }
variable "admin_ip_cidr" { type = string }
variable "tags" { type = map(string) }