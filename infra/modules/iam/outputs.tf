output "instance_profile_arn" { value = aws_iam_instance_profile.this.arn }
output "role_name" { value = aws_iam_role.ec2.name }