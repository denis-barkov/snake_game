resource "aws_cloudwatch_log_group" "app" {
  name              = "/${var.name_prefix}/app"
  retention_in_days = 14
  tags              = merge(var.tags, { Name = "${var.name_prefix}-log-group" })
}