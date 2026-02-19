output "users_table_arn" { value = aws_dynamodb_table.users.arn }
output "snake_checkpoints_table_arn" { value = aws_dynamodb_table.snake_checkpoints.arn }
output "event_ledger_table_arn" { value = aws_dynamodb_table.event_ledger.arn }
output "settings_table_arn" { value = aws_dynamodb_table.settings.arn }
