resource "aws_dynamodb_table" "users" {
  name         = "${var.name_prefix}-users"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "user_id"

  attribute {
    name = "user_id"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-users" })
}

resource "aws_dynamodb_table" "game_progress" {
  name         = "${var.name_prefix}-game_progress"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "user_id"

  attribute {
    name = "user_id"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-game_progress" })
}

resource "aws_dynamodb_table" "settings" {
  name         = "${var.name_prefix}-settings"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "user_id"

  attribute {
    name = "user_id"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-settings" })
}
