TF_DIR=infra
PROFILE=business
PROJECT_TAG?=snake
ENVIRONMENT_TAG?=mvp
ASG_NAME?=$(PROJECT_TAG)-$(ENVIRONMENT_TAG)-asg
APP_REF?=main
BUILD_TARGET?=api/snake_server.cpp
AWS_REGION?=us-east-1
DOMAIN_NAME?=terrariumsnake.com
APP_PORT?=8080
DEPLOY_TIMEOUT_SEC?=1800
GAME_TICK_HZ?=10
GAME_SPECTATOR_HZ?=10
GAME_ENABLE_BROADCAST?=true
GAME_DEBUG_TPS?=false

LOCAL_DYNAMO_ENDPOINT?=http://127.0.0.1:8000
LOCAL_DYNAMO_ENDPOINT_DOCKER?=http://host.docker.internal:8000
LOCAL_DYNAMO_USERS?=snake-local-users
LOCAL_DYNAMO_SNAKES?=snake-local-snakes
LOCAL_DYNAMO_WORLD_CHUNKS?=snake-local-world_chunks
LOCAL_DYNAMO_SNAKE_EVENTS?=snake-local-snake_events
LOCAL_DYNAMO_SETTINGS?=snake-local-settings
LOCAL_DYNAMO_ECONOMY_PARAMS?=snake-local-economy_params
LOCAL_DYNAMO_ECONOMY_PERIOD?=snake-local-economy_period
DOCKER_LOCAL_IMAGE?=snake-local-run:dev
LOCAL_COMPILE_CMD=clang++ -std=c++17 -O2 -pthread api/snake_server.cpp api/protocol/encode_json.cpp api/storage/dynamo_storage.cpp api/storage/storage_factory.cpp api/economy/economy_v1.cpp config/runtime_config.cpp api/world/world.cpp api/world/entities/snake.cpp api/world/entities/food.cpp api/world/systems/movement_system.cpp api/world/systems/collision_system.cpp api/world/systems/spawn_system.cpp -lboost_system -laws-cpp-sdk-dynamodb -laws-cpp-sdk-core -L/usr/local/lib64 -L/usr/local/lib -o snake_server

# Accept both upper/lower-case CLI vars for convenience.
ifneq ($(strip $(branch)),)
APP_REF:=$(branch)
endif
DEPLOY_BRANCH:=$(if $(strip $(BRANCH)),$(strip $(BRANCH)),$(strip $(APP_REF)))

local-dynamo-up:
	docker compose -f docker/dynamodb-local.yml up -d

local-setup:
	@$(MAKE) local-dynamo-up
	@$(MAKE) local-dynamo-create
	@$(MAKE) local-dynamo-seed

local-dynamo-down:
	docker compose -f docker/dynamodb-local.yml down

local-docker-build:
	docker build -f docker/local-run.Dockerfile -t $(DOCKER_LOCAL_IMAGE) .

local-build: local-docker-build
	docker run --rm \
	  -v "$(CURDIR)":/work \
	  -w /work \
	  $(DOCKER_LOCAL_IMAGE) \
	  bash -lc '$(LOCAL_COMPILE_CMD)'

local-dynamo-create:
	DYNAMO_ENDPOINT=$(LOCAL_DYNAMO_ENDPOINT) AWS_REGION=$(AWS_REGION) AWS_ACCESS_KEY_ID=local AWS_SECRET_ACCESS_KEY=local \
	TABLE_USERS=$(LOCAL_DYNAMO_USERS) TABLE_SNAKES=$(LOCAL_DYNAMO_SNAKES) TABLE_WORLD_CHUNKS=$(LOCAL_DYNAMO_WORLD_CHUNKS) TABLE_SNAKE_EVENTS=$(LOCAL_DYNAMO_SNAKE_EVENTS) TABLE_SETTINGS=$(LOCAL_DYNAMO_SETTINGS) TABLE_ECONOMY_PARAMS=$(LOCAL_DYNAMO_ECONOMY_PARAMS) TABLE_ECONOMY_PERIOD=$(LOCAL_DYNAMO_ECONOMY_PERIOD) \
	DYNAMO_TABLE_USERS=$(LOCAL_DYNAMO_USERS) DYNAMO_TABLE_SNAKES=$(LOCAL_DYNAMO_SNAKES) DYNAMO_TABLE_WORLD_CHUNKS=$(LOCAL_DYNAMO_WORLD_CHUNKS) DYNAMO_TABLE_SNAKE_EVENTS=$(LOCAL_DYNAMO_SNAKE_EVENTS) DYNAMO_TABLE_SETTINGS=$(LOCAL_DYNAMO_SETTINGS) DYNAMO_TABLE_ECONOMY_PARAMS=$(LOCAL_DYNAMO_ECONOMY_PARAMS) DYNAMO_TABLE_ECONOMY_PERIOD=$(LOCAL_DYNAMO_ECONOMY_PERIOD) \
	python3 tools/create_local_tables.py

local-dynamo-seed:
	DYNAMO_ENDPOINT=$(LOCAL_DYNAMO_ENDPOINT) AWS_REGION=$(AWS_REGION) AWS_ACCESS_KEY_ID=local AWS_SECRET_ACCESS_KEY=local \
	TABLE_USERS=$(LOCAL_DYNAMO_USERS) TABLE_SNAKES=$(LOCAL_DYNAMO_SNAKES) TABLE_WORLD_CHUNKS=$(LOCAL_DYNAMO_WORLD_CHUNKS) TABLE_SNAKE_EVENTS=$(LOCAL_DYNAMO_SNAKE_EVENTS) TABLE_SETTINGS=$(LOCAL_DYNAMO_SETTINGS) TABLE_ECONOMY_PARAMS=$(LOCAL_DYNAMO_ECONOMY_PARAMS) TABLE_ECONOMY_PERIOD=$(LOCAL_DYNAMO_ECONOMY_PERIOD) \
	DYNAMO_TABLE_USERS=$(LOCAL_DYNAMO_USERS) DYNAMO_TABLE_SNAKES=$(LOCAL_DYNAMO_SNAKES) DYNAMO_TABLE_WORLD_CHUNKS=$(LOCAL_DYNAMO_WORLD_CHUNKS) DYNAMO_TABLE_SNAKE_EVENTS=$(LOCAL_DYNAMO_SNAKE_EVENTS) DYNAMO_TABLE_SETTINGS=$(LOCAL_DYNAMO_SETTINGS) DYNAMO_TABLE_ECONOMY_PARAMS=$(LOCAL_DYNAMO_ECONOMY_PARAMS) DYNAMO_TABLE_ECONOMY_PERIOD=$(LOCAL_DYNAMO_ECONOMY_PERIOD) \
	python3 tools/seed_local.py

local-run-native:
	@echo "local-run-native is disabled (Docker build outputs Linux binary). Use: make local-run"
	@exit 1

local-run: local-run-docker

local-run-docker: local-dynamo-up local-dynamo-create local-docker-build
	-docker rm -f snake-local-run >/dev/null 2>&1 || true
	docker run --rm -it --init \
	  --name snake-local-run \
	  --add-host=host.docker.internal:host-gateway \
	  -p 8080:8080 \
	  -v "$(CURDIR)":/work \
	  -w /work \
	  -e DYNAMO_ENDPOINT=$(LOCAL_DYNAMO_ENDPOINT_DOCKER) \
	  -e AWS_REGION=$(AWS_REGION) \
	  -e DYNAMO_REGION=$(AWS_REGION) \
	  -e AWS_ACCESS_KEY_ID=local \
	  -e AWS_SECRET_ACCESS_KEY=local \
	  -e DYNAMO_TABLE_USERS=$(LOCAL_DYNAMO_USERS) \
	  -e TABLE_USERS=$(LOCAL_DYNAMO_USERS) \
	  -e DYNAMO_TABLE_SNAKES=$(LOCAL_DYNAMO_SNAKES) \
	  -e TABLE_SNAKES=$(LOCAL_DYNAMO_SNAKES) \
	  -e DYNAMO_TABLE_WORLD_CHUNKS=$(LOCAL_DYNAMO_WORLD_CHUNKS) \
	  -e TABLE_WORLD_CHUNKS=$(LOCAL_DYNAMO_WORLD_CHUNKS) \
	  -e DYNAMO_TABLE_SNAKE_EVENTS=$(LOCAL_DYNAMO_SNAKE_EVENTS) \
	  -e TABLE_SNAKE_EVENTS=$(LOCAL_DYNAMO_SNAKE_EVENTS) \
	  -e DYNAMO_TABLE_SETTINGS=$(LOCAL_DYNAMO_SETTINGS) \
	  -e TABLE_SETTINGS=$(LOCAL_DYNAMO_SETTINGS) \
	  -e DYNAMO_TABLE_ECONOMY_PARAMS=$(LOCAL_DYNAMO_ECONOMY_PARAMS) \
	  -e TABLE_ECONOMY_PARAMS=$(LOCAL_DYNAMO_ECONOMY_PARAMS) \
	  -e DYNAMO_TABLE_ECONOMY_PERIOD=$(LOCAL_DYNAMO_ECONOMY_PERIOD) \
	  -e TABLE_ECONOMY_PERIOD=$(LOCAL_DYNAMO_ECONOMY_PERIOD) \
	  -e TICK_HZ=$(GAME_TICK_HZ) \
	  -e SPECTATOR_HZ=$(GAME_SPECTATOR_HZ) \
	  -e ENABLE_BROADCAST=$(GAME_ENABLE_BROADCAST) \
	  -e DEBUG_TPS=$(GAME_DEBUG_TPS) \
	  -e SERVER_BIND_HOST=0.0.0.0 \
	  -e SERVER_BIND_PORT=8080 \
	  $(DOCKER_LOCAL_IMAGE) \
	  bash -lc '$(LOCAL_COMPILE_CMD) && exec ./snake_server serve'

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
	@if [ -z "$(DEPLOY_BRANCH)" ]; then \
	  echo "Pass BRANCH=<git_branch> (or branch=<git_branch>). Example: make aws-code-deploy BRANCH=main"; \
	  exit 1; \
	fi
	DEPLOY_TIMEOUT_SEC=$(DEPLOY_TIMEOUT_SEC) TICK_HZ=$(GAME_TICK_HZ) SPECTATOR_HZ=$(GAME_SPECTATOR_HZ) ENABLE_BROADCAST=$(GAME_ENABLE_BROADCAST) DEBUG_TPS=$(GAME_DEBUG_TPS) AWS_PROFILE=$(PROFILE) AWS_REGION=$(AWS_REGION) PROJECT_TAG=$(PROJECT_TAG) ENVIRONMENT_TAG=$(ENVIRONMENT_TAG) ASG_NAME=$(ASG_NAME) APP_REF=$(DEPLOY_BRANCH) BUILD_TARGET=$(BUILD_TARGET) DOMAIN_NAME=$(DOMAIN_NAME) APP_PORT=$(APP_PORT) bash infra/scripts/deploy_app.sh

aws-apply:
	AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) apply
	@$(MAKE) aws-code-deploy BRANCH=$(DEPLOY_BRANCH)

aws-destroy:
	AWS_PROFILE=$(PROFILE) terraform -chdir=$(TF_DIR) destroy
