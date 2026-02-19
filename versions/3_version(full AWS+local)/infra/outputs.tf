output "public_subnet_id" {
  value = module.vpc.public_subnet_id
}

output "eip_public_ip" {
  value = module.compute.eip_public_ip
}

output "http_url" {
  value = "http://${module.compute.eip_public_ip}:${var.app_control_port}"
}

output "watch_ws_url" {
  value = "wss://${var.domain_name}/ws/watch"
}

output "https_base_url" {
  value = "https://${var.domain_name}"
}

output "route53_record_fqdn" {
  value = aws_route53_record.app_a.fqdn
}

output "compute_asg_name" {
  value = module.compute.asg_name
}

output "ec2_public_ip" {
  value = module.compute.instance_public_ip
}
