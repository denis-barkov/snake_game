output "eip_public_ip" {
  value = length(aws_eip.this) > 0 ? aws_eip.this[0].public_ip : null
}

output "eip_allocation_id" {
  value = length(aws_eip.this) > 0 ? aws_eip.this[0].allocation_id : ""
}

output "asg_name" {
  value = aws_autoscaling_group.this.name
}
