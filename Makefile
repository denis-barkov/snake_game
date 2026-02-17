TF_DIR=infra
PROFILE=business
PROJECT_TAG?=snake
ENVIRONMENT_TAG?=mvp
APP_REF?=main
BUILD_TARGET?=api/snake_server.cpp
AWS_REGION?=us-east-1

aws-init:
	AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) init -upgrade

aws-ssh-refresh-no-applying:
	@IP=$$(curl -fsS https://checkip.amazonaws.com | tr -d '\r\n'); \
	if [ -z "$$IP" ]; then echo "Could not detect public IP"; exit 1; fi; \
	if [ -z "$(KEY)" ]; then \
	  echo "Pass KEY=<key_name> (example: make aws-ssh-refresh KEY=my-key)"; \
	  exit 1; \
	fi; \
	{ \
	  echo "allow_ssh = true"; \
	  echo "admin_ip_cidr = \"$$IP/32\""; \
	  echo "ssh_key_name = \"$(KEY)\""; \
	} > $(TF_DIR)/ssh.auto.tfvars; \
	echo "Updated $(TF_DIR)/ssh.auto.tfvars with $$IP/32 and key '$(KEY)'"

aws-ssh-refresh:
	@$(MAKE) aws-ssh-refresh-no-applying
	@$(MAKE) aws-apply

aws-ssh-disable:
	@{ \
	  echo "allow_ssh = false"; \
	  echo "admin_ip_cidr = \"0.0.0.0/32\""; \
	} > $(TF_DIR)/ssh.auto.tfvars; \
	echo "SSH disabled in $(TF_DIR)/ssh.auto.tfvars"
	@$(MAKE) aws-apply

aws-asg-refresh:
	@ASG_NAME=$$(AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) output -raw compute_asg_name); \
	if [ -z "$$ASG_NAME" ]; then echo "Could not read compute_asg_name output"; exit 1; fi; \
	AWS_PROFILE=$(PROFILE) aws autoscaling start-instance-refresh --auto-scaling-group-name "$$ASG_NAME"

aws-asg-refresh-status:
	@ASG_NAME=$$(AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) output -raw compute_asg_name); \
	if [ -z "$$ASG_NAME" ]; then echo "Could not read compute_asg_name output"; exit 1; fi; \
	AWS_PROFILE=$(PROFILE) aws autoscaling describe-instance-refreshes --auto-scaling-group-name "$$ASG_NAME" --max-records 1

aws-eip-attach:
	@ASG_NAME=$$(AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) output -raw compute_asg_name); \
	EIP_IP=$$(AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) output -raw eip_public_ip); \
	if [ -z "$$ASG_NAME" ] || [ -z "$$EIP_IP" ]; then echo "Missing ASG name or EIP output"; exit 1; fi; \
	INSTANCE_ID=$$(AWS_PROFILE=$(PROFILE) aws autoscaling describe-auto-scaling-groups --auto-scaling-group-name "$$ASG_NAME" --query 'AutoScalingGroups[0].Instances[0].InstanceId' --output text); \
	ALLOC_ID=$$(AWS_PROFILE=$(PROFILE) aws ec2 describe-addresses --public-ips "$$EIP_IP" --query 'Addresses[0].AllocationId' --output text); \
	if [ -z "$$INSTANCE_ID" ] || [ "$$INSTANCE_ID" = "None" ] || [ -z "$$ALLOC_ID" ] || [ "$$ALLOC_ID" = "None" ]; then echo "Could not resolve instance or EIP allocation id"; exit 1; fi; \
	AWS_PROFILE=$(PROFILE) aws ec2 associate-address --allocation-id "$$ALLOC_ID" --instance-id "$$INSTANCE_ID" --allow-reassociation; \
	echo "Attached EIP $$EIP_IP ($$ALLOC_ID) to $$INSTANCE_ID"

aws-plan:
	AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) plan -input=false

aws-code-deploy:
	AWS_PROFILE=$(PROFILE) AWS_REGION=$(AWS_REGION) PROJECT_TAG=$(PROJECT_TAG) ENVIRONMENT_TAG=$(ENVIRONMENT_TAG) APP_REF=$(APP_REF) BUILD_TARGET=$(BUILD_TARGET) bash infra/scripts/deploy_app.sh

aws-apply:
	AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) apply
	@$(MAKE) aws-code-deploy

aws-destroy:
	AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) destroy
