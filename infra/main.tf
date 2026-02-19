provider "aws" {
  region  = var.aws_region
  profile = "business"
}

locals {
  tags = {
    project     = var.project
    environment = var.environment
    owner       = var.owner
  }

  route53_zone_name_fqdn = endswith(var.route53_zone_name, ".") ? var.route53_zone_name : "${var.route53_zone_name}."
}

module "vpc" {
  source = "./modules/vpc"

  vpc_cidr    = "10.0.0.0/16"
  subnet_cidr = "10.0.1.0/24"
  az          = var.az
  name_prefix = "${var.project}-${var.environment}"

  tags = local.tags
}

module "security" {
  source = "./modules/security"

  vpc_id        = module.vpc.vpc_id
  name_prefix   = "${var.project}-${var.environment}"
  allow_ssh     = var.allow_ssh
  admin_ip_cidr = var.admin_ip_cidr
  tags          = local.tags
}

module "iam" {
  source = "./modules/iam"

  name_prefix = "${var.project}-${var.environment}"
  tags        = local.tags

  dynamodb_table_arns = [
    module.dynamodb.users_table_arn,
    module.dynamodb.snake_checkpoints_table_arn,
    module.dynamodb.event_ledger_table_arn,
    module.dynamodb.settings_table_arn
  ]

  cloudwatch_log_group_arn = module.observability.log_group_arn

  # Needed for EIP attach in user-data:
  allow_eip_association = true
}

module "dynamodb" {
  source = "./modules/dynamodb"

  name_prefix = "${var.project}-${var.environment}"
  tags        = local.tags
}

module "observability" {
  source = "./modules/observability"

  name_prefix = "${var.project}-${var.environment}"
  tags        = local.tags
}

module "compute" {
  source = "./modules/compute_asg"

  name_prefix          = "${var.project}-${var.environment}"
  vpc_id               = module.vpc.vpc_id
  subnet_id            = module.vpc.public_subnet_id
  security_group_id    = module.security.ec2_sg_id
  instance_profile_arn = module.iam.instance_profile_arn

  instance_type = var.instance_type
  ebs_volume_gb = 30

  asg_min     = 1
  asg_desired = 1
  asg_max     = 1

  # App deploy settings (you can point to your repo/binary)
  app_git_repo     = var.app_git_repo
  app_git_ref      = var.app_git_ref
  app_build_target = var.app_build_target
  app_listen_port  = var.app_control_port
  app_env = merge(var.app_env, {
    AWS_REGION                     = var.aws_region
    DYNAMO_REGION                  = var.aws_region
    DYNAMO_TABLE_USERS             = "${var.project}-${var.environment}-users"
    DYNAMO_TABLE_SNAKE_CHECKPOINTS = "${var.project}-${var.environment}-snake_checkpoints"
    DYNAMO_TABLE_EVENT_LEDGER      = "${var.project}-${var.environment}-event_ledger"
    DYNAMO_TABLE_SETTINGS          = "${var.project}-${var.environment}-settings"
  })
  domain_name       = var.domain_name
  letsencrypt_email = var.letsencrypt_email
  allow_ssh         = var.allow_ssh
  ssh_key_name      = var.ssh_key_name

  cloudwatch_log_group_name = module.observability.log_group_name

  # Elastic IP for MVP
  allocate_eip = true
  tags         = local.tags
}

# Save EIP allocation id into SSM for user-data (optional but useful)
resource "aws_ssm_parameter" "eip_allocation_id" {
  name  = "/${var.project}/${var.environment}/eip_allocation_id"
  type  = "String"
  value = module.compute.eip_allocation_id
  tags  = local.tags
}

data "aws_route53_zone" "existing" {
  name         = local.route53_zone_name_fqdn
  private_zone = false
}

resource "aws_route53_record" "app_a" {
  zone_id = data.aws_route53_zone.existing.zone_id
  name    = var.domain_name
  type    = "A"
  ttl     = 300
  records = [module.compute.eip_public_ip]
}
