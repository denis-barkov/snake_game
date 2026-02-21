#!/usr/bin/env bash
set -euo pipefail

REGION="${AWS_REGION:-us-east-1}"
PROJECT_TAG="${PROJECT_TAG:-snake}"
ENVIRONMENT_TAG="${ENVIRONMENT_TAG:-mvp}"
ASG_NAME="${ASG_NAME:-${PROJECT_TAG}-${ENVIRONMENT_TAG}-asg}"
APP_REF="${APP_REF:-main}"
BUILD_TARGET="${BUILD_TARGET:-api/snake_server.cpp}"
DOMAIN_NAME="${DOMAIN_NAME:-terrariumsnake.com}"
APP_PORT="${APP_PORT:-8080}"
TICK_HZ="${TICK_HZ:-10}"
SPECTATOR_HZ="${SPECTATOR_HZ:-10}"
ENABLE_BROADCAST="${ENABLE_BROADCAST:-true}"
DEBUG_TPS="${DEBUG_TPS:-false}"
POLL_ATTEMPTS="${POLL_ATTEMPTS:-20}"
POLL_SLEEP_SECONDS="${POLL_SLEEP_SECONDS:-15}"
SSM_POLL_ATTEMPTS="${SSM_POLL_ATTEMPTS:-20}"
SSM_POLL_SLEEP_SECONDS="${SSM_POLL_SLEEP_SECONDS:-15}"

find_instance() {
  local ids
  ids="$(
    aws ec2 describe-instances \
      --region "$REGION" \
      --filters \
        "Name=tag:project,Values=${PROJECT_TAG}" \
        "Name=tag:environment,Values=${ENVIRONMENT_TAG}" \
        "Name=instance-state-name,Values=running" \
      --query 'Reservations[].Instances[].InstanceId' \
      --output text
  )"

  if [[ -n "$ids" && "$ids" != "None" ]]; then
    echo "$ids"
    return 0
  fi

  # Fallback: find by ASG system tag if project/environment tags are missing or mismatched.
  aws ec2 describe-instances \
    --region "$REGION" \
    --filters \
      "Name=tag:aws:autoscaling:groupName,Values=${ASG_NAME}" \
      "Name=instance-state-name,Values=running" \
    --query 'Reservations[].Instances[].InstanceId' \
    --output text
}

INSTANCE_IDS=""
for _ in $(seq 1 "$POLL_ATTEMPTS"); do
  INSTANCE_IDS="$(find_instance || true)"
  if [[ -n "$INSTANCE_IDS" && "$INSTANCE_IDS" != "None" ]]; then
    break
  fi
  sleep "$POLL_SLEEP_SECONDS"
done

if [[ -z "$INSTANCE_IDS" || "$INSTANCE_IDS" == "None" ]]; then
  echo "No running EC2 found by tags project=${PROJECT_TAG}, environment=${ENVIRONMENT_TAG} or ASG=${ASG_NAME} in ${REGION}."
  exit 2
fi

INSTANCE_COUNT="$(wc -w <<<"$INSTANCE_IDS" | tr -d ' ')"
if [[ "$INSTANCE_COUNT" -ne 1 ]]; then
  echo "Expected exactly 1 running instance for tags project=${PROJECT_TAG}, environment=${ENVIRONMENT_TAG}; found: ${INSTANCE_IDS}"
  exit 3
fi

INSTANCE_ID="$INSTANCE_IDS"
echo "Deploying app to ${INSTANCE_ID} (project=${PROJECT_TAG}, environment=${ENVIRONMENT_TAG})"

wait_for_ssm_online() {
  local instance_id="$1"
  local ping=""
  for _ in $(seq 1 "$SSM_POLL_ATTEMPTS"); do
    ping="$(
      aws ssm describe-instance-information \
        --region "$REGION" \
        --filters "Key=InstanceIds,Values=${instance_id}" \
        --query 'InstanceInformationList[0].PingStatus' \
        --output text 2>/dev/null || true
    )"
    if [[ "$ping" == "Online" ]]; then
      return 0
    fi
    sleep "$SSM_POLL_SLEEP_SECONDS"
  done
  return 1
}

if ! wait_for_ssm_online "$INSTANCE_ID"; then
  echo "Instance ${INSTANCE_ID} is not SSM Online yet in ${REGION}. Try again in a few minutes."
  exit 5
fi

COMMAND_ID="$(
  aws ssm send-command \
    --region "$REGION" \
    --instance-ids "$INSTANCE_ID" \
    --document-name "AWS-RunShellScript" \
    --comment "snake deploy from ${APP_REF}" \
    --parameters "commands=[
\"set -euo pipefail\",
\"dnf -y install git clang boost-devel cmake gcc-c++ libcurl-devel openssl-devel zlib-devel >/dev/null\",
\"if [ ! -f /usr/local/lib64/libaws-cpp-sdk-dynamodb.so ] && [ ! -f /usr/local/lib/libaws-cpp-sdk-dynamodb.so ]; then if [ ! -d /opt/aws-sdk-cpp ]; then git clone --depth 1 --branch 1.11.676 --recurse-submodules https://github.com/aws/aws-sdk-cpp.git /opt/aws-sdk-cpp; else cd /opt/aws-sdk-cpp; git fetch --tags --force; git checkout 1.11.676; git submodule sync --recursive; git submodule update --init --recursive; fi; cmake -S /opt/aws-sdk-cpp -B /opt/aws-sdk-cpp/build -DBUILD_ONLY='dynamodb' -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF >/dev/null; cmake --build /opt/aws-sdk-cpp/build -j2 >/dev/null; cmake --install /opt/aws-sdk-cpp/build >/dev/null; echo -e '/usr/local/lib64\\n/usr/local/lib' >/etc/ld.so.conf.d/aws-sdk-cpp.conf; ldconfig || true; fi\",
\"mkdir -p /opt/snake\",
\"if [ ! -d /opt/snake/repo ]; then echo Missing /opt/snake/repo; exit 1; fi\",
\"cd /opt/snake/repo\",
\"git fetch --all --prune\",
\"git checkout ${APP_REF}\",
\"git pull --ff-only origin ${APP_REF}\",
\"if [ -f /opt/snake/repo/index.html ]; then mkdir -p /var/www/snake; cp -f /opt/snake/repo/index.html /var/www/snake/index.html; fi\",
\"if [ -d /opt/snake/repo/src ]; then rm -rf /var/www/snake/src; cp -a /opt/snake/repo/src /var/www/snake/src; fi\",
\"if [ ! -f /var/www/snake/index.html ]; then echo Missing /var/www/snake/index.html after deploy; exit 1; fi\",
\"if [ -f /var/www/snake/index.html ]; then sed -i 's|const API = \\\"http://127.0.0.1:8080\\\";|const API = window.location.origin;|' /var/www/snake/index.html || true; fi\",
\"chmod 755 /var/www/snake || true\",
\"chmod 644 /var/www/snake/index.html || true\",
\"if [ -d /var/www/snake/src ]; then find /var/www/snake/src -type d -exec chmod 755 {} \\;; find /var/www/snake/src -type f -exec chmod 644 {} \\;; fi\",
\"clang++ -std=c++17 -O2 -pthread ${BUILD_TARGET} api/protocol/encode_json.cpp api/storage/dynamo_storage.cpp api/storage/storage_factory.cpp config/runtime_config.cpp -o /opt/snake/snake_server -lboost_system -laws-cpp-sdk-dynamodb -laws-cpp-sdk-core -L/usr/local/lib64 -L/usr/local/lib\",
\"cat > /etc/snake.env <<'EOF_ENV'\",
\"AWS_REGION=${REGION}\",
\"DYNAMO_REGION=${REGION}\",
\"DYNAMO_TABLE_USERS=${PROJECT_TAG}-${ENVIRONMENT_TAG}-users\",
\"DYNAMO_TABLE_SNAKE_CHECKPOINTS=${PROJECT_TAG}-${ENVIRONMENT_TAG}-snake_checkpoints\",
\"DYNAMO_TABLE_EVENT_LEDGER=${PROJECT_TAG}-${ENVIRONMENT_TAG}-event_ledger\",
\"DYNAMO_TABLE_SETTINGS=${PROJECT_TAG}-${ENVIRONMENT_TAG}-settings\",
\"SNAKE_W=40\",
\"SNAKE_H=20\",
\"SNAKE_MAX_PER_USER=3\",
\"TICK_HZ=${TICK_HZ}\",
\"SPECTATOR_HZ=${SPECTATOR_HZ}\",
\"ENABLE_BROADCAST=${ENABLE_BROADCAST}\",
\"DEBUG_TPS=${DEBUG_TPS}\",
\"EOF_ENV\",
\"chmod 0644 /etc/snake.env\",
\"if [ -f /opt/snake/repo/tools/snake-admin.sh ]; then install -m 0755 /opt/snake/repo/tools/snake-admin.sh /usr/local/bin/snake-admin; else echo Missing /opt/snake/repo/tools/snake-admin.sh; exit 1; fi\",
\"cat > /etc/systemd/system/snake-seed.service <<'EOF_SNAKE_SEED'\",
\"[Unit]\",
\"Description=Seed Snake DynamoDB data\",
\"After=network-online.target\",
\"Wants=network-online.target\",
\"[Service]\",
\"Type=oneshot\",
\"ExecStart=/usr/local/bin/snake-admin seed\",
\"EOF_SNAKE_SEED\",
\"cat > /etc/systemd/system/snake-reset.service <<'EOF_SNAKE_RESET'\",
\"[Unit]\",
\"Description=Reset Snake DynamoDB data\",
\"After=network-online.target\",
\"Wants=network-online.target\",
\"[Service]\",
\"Type=oneshot\",
\"ExecStart=/usr/local/bin/snake-admin reset\",
\"EOF_SNAKE_RESET\",
\"cat > /etc/systemd/system/snake-reset-seed.service <<'EOF_SNAKE_RESET_SEED'\",
\"[Unit]\",
\"Description=Reset and seed Snake DynamoDB data\",
\"After=network-online.target\",
\"Wants=network-online.target\",
\"[Service]\",
\"Type=oneshot\",
\"ExecStart=/usr/local/bin/snake-admin reset-seed\",
\"EOF_SNAKE_RESET_SEED\",
\"cat > /etc/systemd/system/snake-reload.service <<'EOF_SNAKE_RELOAD'\",
\"[Unit]\",
\"Description=Reload Snake state from DynamoDB (no restart)\",
\"After=network-online.target\",
\"Wants=network-online.target\",
\"[Service]\",
\"Type=oneshot\",
\"ExecStart=/usr/local/bin/snake-admin reload\",
\"EOF_SNAKE_RELOAD\",
\"cat > /etc/systemd/system/snake-seed-reload.service <<'EOF_SNAKE_SEED_RELOAD'\",
\"[Unit]\",
\"Description=Seed Snake DynamoDB data and reload running server\",
\"After=network-online.target\",
\"Wants=network-online.target\",
\"[Service]\",
\"Type=oneshot\",
\"ExecStart=/usr/local/bin/snake-admin seed-reload\",
\"EOF_SNAKE_SEED_RELOAD\",
\"mkdir -p /etc/systemd/system/snake.service.d\",
\"cat > /etc/systemd/system/snake.service.d/limits.conf <<'EOF_SNAKE_LIMITS'\",
\"[Service]\",
\"LimitNOFILE=100000\",
\"EOF_SNAKE_LIMITS\",
\"if ! command -v caddy >/dev/null 2>&1; then dnf -y install dnf-plugins-core ca-certificates curl tar >/dev/null || true; dnf config-manager --add-repo https://dl.cloudsmith.io/public/caddy/stable/rpm.repo >/dev/null 2>&1 || true; rpm --import https://dl.cloudsmith.io/public/caddy/stable/gpg.key >/dev/null 2>&1 || true; dnf -y install caddy >/dev/null 2>&1 || true; fi\",
\"if ! command -v caddy >/dev/null 2>&1; then curl -fsSL 'https://caddyserver.com/api/download?os=linux&arch=arm64&p=github.com/caddyserver/caddy/v2' -o /usr/local/bin/caddy && chmod +x /usr/local/bin/caddy; fi\",
\"if [ -x /usr/local/bin/caddy ]; then mkdir -p /etc/caddy /var/lib/caddy /var/log/caddy; cat > /etc/systemd/system/caddy.service <<'EOF_CADDY_UNIT'\",
\"[Unit]\",
\"Description=Caddy web server\",
\"After=network-online.target\",
\"Wants=network-online.target\",
\"[Service]\",
\"User=root\",
\"Group=root\",
\"ExecStart=/usr/local/bin/caddy run --environ --config /etc/caddy/Caddyfile\",
\"ExecReload=/usr/local/bin/caddy reload --config /etc/caddy/Caddyfile\",
\"Restart=on-failure\",
\"LimitNOFILE=1048576\",
\"[Install]\",
\"WantedBy=multi-user.target\",
\"EOF_CADDY_UNIT\",
\"fi\",
\"if command -v caddy >/dev/null 2>&1; then cat > /etc/caddy/Caddyfile <<'EOF_CADDY'\",
\"${DOMAIN_NAME} {\",
\"  encode zstd gzip\",
\"  @sse path /game/stream\",
\"  handle @sse {\",
\"    reverse_proxy 127.0.0.1:${APP_PORT} {\",
\"      flush_interval -1\",
\"    }\",
\"  }\",
\"  @api path /auth/* /me/* /snakes/* /game/*\",
\"  handle @api {\",
\"    reverse_proxy 127.0.0.1:${APP_PORT}\",
\"  }\",
\"  root * /var/www/snake\",
\"  file_server\",
\"}\",
\"EOF_CADDY\",
\"fi\",
\"dnf -y install nginx >/dev/null || true\",
\"rm -f /etc/nginx/conf.d/*.conf /etc/nginx/default.d/*.conf; cat > /etc/nginx/conf.d/snake.conf <<'EOF_NGINX'\",
\"server {\",
\"  listen 80 default_server;\",
\"  server_name ${DOMAIN_NAME};\",
\"  root /var/www/snake;\",
\"  index index.html;\",
\"  location = / { try_files /index.html =404; }\",
\"  location = /index.html { try_files /index.html =404; }\",
\"  location = /game/stream {\",
\"    proxy_pass http://127.0.0.1:${APP_PORT};\",
\"    proxy_http_version 1.1;\",
\"    proxy_set_header Connection \\\"\\\";\",
\"    proxy_buffering off;\",
\"    proxy_cache off;\",
\"    proxy_read_timeout 3600;\",
\"    proxy_send_timeout 3600;\",
\"    send_timeout 3600;\",
\"  }\",
\"  location /auth/ { proxy_pass http://127.0.0.1:${APP_PORT}; }\",
\"  location /me/ { proxy_pass http://127.0.0.1:${APP_PORT}; }\",
\"  location /snakes/ { proxy_pass http://127.0.0.1:${APP_PORT}; }\",
\"  location /game/ { proxy_pass http://127.0.0.1:${APP_PORT}; }\",
\"  location / { try_files \\\$uri \\\$uri/ /index.html; }\",
\"}\",
\"EOF_NGINX\",
\"systemctl daemon-reload || true\",
\"systemctl restart snake\",
\"systemctl is-active snake\",
\"if command -v caddy >/dev/null 2>&1; then systemctl daemon-reload || true; systemctl stop nginx >/dev/null 2>&1 || true; systemctl disable nginx >/dev/null 2>&1 || true; systemctl enable --now caddy || true; fi\",
\"if command -v caddy >/dev/null 2>&1 && systemctl is-active --quiet caddy; then systemctl is-active caddy; else nginx -t; systemctl enable --now nginx; systemctl is-active nginx; fi\"
]" \
    --query 'Command.CommandId' \
    --output text
)"

echo "SSM command id: ${COMMAND_ID}"

dump_ssm_output() {
  aws ssm get-command-invocation \
    --region "$REGION" \
    --command-id "$COMMAND_ID" \
    --instance-id "$INSTANCE_ID" \
    --query '{Status:Status,StatusDetails:StatusDetails,ResponseCode:ResponseCode,StdOut:StandardOutputContent,StdErr:StandardErrorContent}' \
    --output json || true
  aws ssm list-command-invocations \
    --region "$REGION" \
    --command-id "$COMMAND_ID" \
    --details \
    --query 'CommandInvocations[0].CommandPlugins[].{Name:Name,Status:Status,ResponseCode:ResponseCode,Output:Output}' \
    --output text || true
}

MAX_WAIT_SEC="${DEPLOY_TIMEOUT_SEC:-2400}"
POLL_SEC=15
ELAPSED=0
STATUS="Pending"

while true; do
  STATUS="$(
    aws ssm get-command-invocation \
      --region "$REGION" \
      --command-id "$COMMAND_ID" \
      --instance-id "$INSTANCE_ID" \
      --query 'Status' \
      --output text 2>/dev/null || echo "Pending"
  )"

  echo "SSM status: ${STATUS} (${ELAPSED}s/${MAX_WAIT_SEC}s)"

  case "$STATUS" in
    Success)
      break
      ;;
    Failed|Cancelled|TimedOut)
      echo "SSM command failed. Fetching command output..."
      dump_ssm_output
      exit 4
      ;;
    Pending|InProgress|Delayed)
      if [ "$ELAPSED" -ge "$MAX_WAIT_SEC" ]; then
        echo "SSM command timed out after ${MAX_WAIT_SEC}s. Fetching command output..."
        dump_ssm_output
        exit 4
      fi
      sleep "$POLL_SEC"
      ELAPSED=$((ELAPSED + POLL_SEC))
      ;;
    *)
      echo "SSM command reached unexpected status '${STATUS}'. Fetching command output..."
      dump_ssm_output
      exit 4
      ;;
  esac
done

echo "Deploy status: ${STATUS}"

if [[ "$STATUS" != "Success" ]]; then
  dump_ssm_output
  exit 4
fi

echo "Deploy completed."
