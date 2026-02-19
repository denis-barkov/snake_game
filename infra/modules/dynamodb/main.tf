resource "aws_dynamodb_table" "users" {
  name         = "${var.name_prefix}-users"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "user_id"

  attribute {
    name = "user_id"
    type = "S"
  }
  attribute {
    name = "username"
    type = "S"
  }

  global_secondary_index {
    name            = "gsi_username"
    hash_key        = "username"
    projection_type = "ALL"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-users" })
}

resource "aws_dynamodb_table" "snake_checkpoints" {
  name         = "${var.name_prefix}-snake_checkpoints"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "snake_id"
  range_key    = "ts"

  attribute {
    name = "snake_id"
    type = "S"
  }
  attribute {
    name = "ts"
    type = "N"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-snake_checkpoints" })
}

resource "aws_dynamodb_table" "event_ledger" {
  name         = "${var.name_prefix}-event_ledger"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "pk"
  range_key    = "sk"

  attribute {
    name = "pk"
    type = "S"
  }
  attribute {
    name = "sk"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-event_ledger" })
}

resource "aws_dynamodb_table" "settings" {
  name         = "${var.name_prefix}-settings"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "pk"
  range_key    = "sk"

  attribute {
    name = "pk"
    type = "S"
  }
  attribute {
    name = "sk"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-settings" })
}
