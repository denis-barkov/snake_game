variable "name_prefix" { type = string }
variable "tags" { type = map(string) }

variable "dynamodb_table_arns" {
  type = list(string)
}

variable "cloudwatch_log_group_arn" {
  type = string
}

variable "allow_eip_association" {
  type    = bool
  default = true
}