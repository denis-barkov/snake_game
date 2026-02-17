data "aws_ami" "al2023_arm" {
  most_recent = true
  owners      = ["amazon"]

  filter {
    name   = "name"
    values = ["al2023-ami-*-kernel-*-arm64"]
  }
  filter {
    name   = "architecture"
    values = ["arm64"]
  }
}

resource "aws_eip" "this" {
  count  = var.allocate_eip ? 1 : 0
  domain = "vpc"
  tags   = merge(var.tags, { Name = "${var.name_prefix}-eip" })
}

locals {
  env_lines = join("\n", [for k, v in var.app_env : "${k}=${v}"])
  selected_ssh_key_name = (
    var.ssh_key_name != null && trimspace(var.ssh_key_name) != ""
  ) ? trimspace(var.ssh_key_name) : null
  cwagent_config_rendered = templatefile("${path.module}/cwagent.json.tftpl", {
    log_group_name = var.cloudwatch_log_group_name
  })
  user_data_rendered = templatefile("${path.module}/user_data.sh.tftpl", {
    name_prefix        = var.name_prefix
    app_git_repo       = var.app_git_repo
    app_git_ref        = var.app_git_ref
    app_build_target   = var.app_build_target
    app_port           = tostring(var.app_listen_port)
    env_lines          = local.env_lines
    eip_allocation_id  = var.allocate_eip ? aws_eip.this[0].allocation_id : ""
    cwagent_config_b64 = base64encode(local.cwagent_config_rendered)
    domain_name        = var.domain_name
    letsencrypt_email  = var.letsencrypt_email
  })
}

resource "aws_launch_template" "this" {
  name_prefix   = "${var.name_prefix}-lt-"
  image_id      = data.aws_ami.al2023_arm.id
  instance_type = var.instance_type
  key_name      = local.selected_ssh_key_name

  lifecycle {
    precondition {
      condition     = !var.allow_ssh || local.selected_ssh_key_name != null
      error_message = "SSH is enabled but ssh_key_name is not set. Provide var.ssh_key_name (or use make aws-ssh-refresh KEY=...)."
    }
  }

  vpc_security_group_ids = [var.security_group_id]

  iam_instance_profile {
    arn = var.instance_profile_arn
  }

  block_device_mappings {
    device_name = "/dev/xvda"
    ebs {
      volume_size           = var.ebs_volume_gb
      volume_type           = "gp3"
      delete_on_termination = true
    }
  }

  user_data = base64encode(local.user_data_rendered)

  tag_specifications {
    resource_type = "instance"
    tags          = merge(var.tags, { Name = "${var.name_prefix}-instance" })
  }
  tag_specifications {
    resource_type = "volume"
    tags          = merge(var.tags, { Name = "${var.name_prefix}-volume" })
  }
}

resource "aws_autoscaling_group" "this" {
  name                = "${var.name_prefix}-asg"
  min_size            = var.asg_min
  desired_capacity    = var.asg_desired
  max_size            = var.asg_max
  vpc_zone_identifier = [var.subnet_id]
  health_check_type   = "EC2"

  launch_template {
    id      = aws_launch_template.this.id
    version = "$Latest"
  }

  tag {
    key                 = "Name"
    value               = "${var.name_prefix}-asg"
    propagate_at_launch = true
  }

  dynamic "tag" {
    for_each = var.tags
    content {
      key                 = tag.key
      value               = tag.value
      propagate_at_launch = true
    }
  }

  instance_refresh {
    strategy = "Rolling"

    preferences {
      min_healthy_percentage = 0
    }
  }
}

# Alarms (MVP)
resource "aws_cloudwatch_metric_alarm" "cpu_high" {
  alarm_name          = "${var.name_prefix}-cpu-high"
  comparison_operator = "GreaterThanThreshold"
  evaluation_periods  = 2
  metric_name         = "CPUUtilization"
  namespace           = "AWS/EC2"
  period              = 60
  statistic           = "Average"
  threshold           = 70

  dimensions = {
    AutoScalingGroupName = aws_autoscaling_group.this.name
  }

  tags = var.tags
}

resource "aws_cloudwatch_metric_alarm" "instance_status" {
  alarm_name          = "${var.name_prefix}-status-check"
  comparison_operator = "GreaterThanThreshold"
  evaluation_periods  = 2
  metric_name         = "StatusCheckFailed"
  namespace           = "AWS/EC2"
  period              = 60
  statistic           = "Maximum"
  threshold           = 0

  dimensions = {
    AutoScalingGroupName = aws_autoscaling_group.this.name
  }

  tags = var.tags
}
