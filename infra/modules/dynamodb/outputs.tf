output "users_table_arn" { value = aws_dynamodb_table.users.arn }
output "snakes_table_arn" { value = aws_dynamodb_table.snakes.arn }
output "world_chunks_table_arn" { value = aws_dynamodb_table.world_chunks.arn }
output "snake_events_table_arn" { value = aws_dynamodb_table.snake_events.arn }
output "settings_table_arn" { value = aws_dynamodb_table.settings.arn }
output "economy_params_table_arn" { value = aws_dynamodb_table.economy_params.arn }
output "economy_period_table_arn" { value = aws_dynamodb_table.economy_period.arn }
