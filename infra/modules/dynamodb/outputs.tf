output "users_table_arn" { value = aws_dynamodb_table.users.arn }
output "game_progress_table_arn" { value = aws_dynamodb_table.game_progress.arn }
output "settings_table_arn" { value = aws_dynamodb_table.settings.arn }