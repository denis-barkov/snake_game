# AWS MVP Environment (Terraform)

This provisions an MVP environment for the Snake WebSocket server:
- VPC (10.0.0.0/16), 1 public subnet, IGW, public route table
- EC2 in AutoScalingGroup (min=1 desired=1 max=2), Amazon Linux 2023 ARM, t4g.small
- Public EIP attached (MVP; best-effort when ASG scales beyond 1)
- DynamoDB tables (PAY_PER_REQUEST): users, game_progress, settings
- CloudWatch Log Group + CloudWatch Agent shipping /var/log/snake_server.log
- CPU + Status alarms (MVP)
- SSM Managed Instance Core (recommended instead of SSH)

## Prereqs
- Terraform >= 1.6
- AWS credentials configured (AWS_PROFILE or env vars)
- Optional: Homebrew `awscli`

## Configure variables
Create `infra/terraform.tfvars`:

```hcl
project     = "snake"
environment = "mvp"
owner       = "denis"
aws_region  = "us-east-1"
az          = "us-east-1a"

# App deployment
app_git_repo     = "https://github.com/YOUR_ORG/YOUR_REPO.git"
app_git_ref      = "main"
app_build_target = "api/snake_server_ws.cpp"

# Security
allow_ssh     = false
admin_ip_cidr = "YOUR_PUBLIC_IP/32"

# App port (control + HTTP)
app_control_port = 8080

app_env = {
  SNAKE_W = "40"
  SNAKE_H = "20"
  SNAKE_TICK_MS = "150"
  SNAKE_MAX_PER_USER = "3"
}