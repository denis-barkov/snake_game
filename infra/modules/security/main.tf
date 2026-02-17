resource "aws_security_group" "ec2" {
  name        = "${var.name_prefix}-ec2-sg"
  description = "EC2 SG for snake server"
  vpc_id      = var.vpc_id
  tags        = merge(var.tags, { Name = "${var.name_prefix}-ec2-sg" })

  # HTTP (required for Let's Encrypt HTTP-01 challenge)
  ingress {
    from_port   = 80
    to_port     = 80
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  # HTTPS (WebSocket over TLS)
  ingress {
    from_port   = 443
    to_port     = 443
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  dynamic "ingress" {
    for_each = var.allow_ssh ? [1] : []
    content {
      from_port   = 22
      to_port     = 22
      protocol    = "tcp"
      cidr_blocks = [var.admin_ip_cidr]
    }
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}