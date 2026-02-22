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

resource "aws_dynamodb_table" "snakes" {
  name         = "${var.name_prefix}-snakes"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "snake_id"

  attribute {
    name = "snake_id"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-snakes" })
}

resource "aws_dynamodb_table" "world_chunks" {
  name         = "${var.name_prefix}-world_chunks"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "chunk_id"

  attribute {
    name = "chunk_id"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-world_chunks" })
}

resource "aws_dynamodb_table" "snake_events" {
  name         = "${var.name_prefix}-snake_events"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "snake_id"
  range_key    = "event_id"

  attribute {
    name = "snake_id"
    type = "S"
  }
  attribute {
    name = "event_id"
    type = "S"
  }

  ttl {
    attribute_name = "ttl"
    enabled        = false
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-snake_events" })
}

resource "aws_dynamodb_table" "settings" {
  name         = "${var.name_prefix}-settings"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "settings_id"

  attribute {
    name = "settings_id"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-settings" })
}

resource "aws_dynamodb_table" "economy_params" {
  name         = "${var.name_prefix}-economy_params"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "params_id"

  attribute {
    name = "params_id"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-economy_params" })
}

resource "aws_dynamodb_table" "economy_period" {
  name         = "${var.name_prefix}-economy_period"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "period_key"

  attribute {
    name = "period_key"
    type = "S"
  }

  tags = merge(var.tags, { Name = "${var.name_prefix}-economy_period" })
}
