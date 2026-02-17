#!/usr/bin/env bash
set -euo pipefail

REGION="${AWS_REGION:-us-east-1}"
PROJECT_TAG="${PROJECT_TAG:-snake}"
ENVIRONMENT_TAG="${ENVIRONMENT_TAG:-mvp}"
APP_REF="${APP_REF:-main}"
BUILD_TARGET="${BUILD_TARGET:-api/snake_server.cpp}"
DOMAIN_NAME="${DOMAIN_NAME:-terrariumsnake.com}"
APP_PORT="${APP_PORT:-8080}"
POLL_ATTEMPTS="${POLL_ATTEMPTS:-20}"
POLL_SLEEP_SECONDS="${POLL_SLEEP_SECONDS:-15}"
SSM_POLL_ATTEMPTS="${SSM_POLL_ATTEMPTS:-20}"
SSM_POLL_SLEEP_SECONDS="${SSM_POLL_SLEEP_SECONDS:-15}"

find_instance() {
  aws ec2 describe-instances \
    --region "$REGION" \
    --filters \
      "Name=tag:project,Values=${PROJECT_TAG}" \
      "Name=tag:environment,Values=${ENVIRONMENT_TAG}" \
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
  echo "No running EC2 found with tags project=${PROJECT_TAG}, environment=${ENVIRONMENT_TAG} in ${REGION}."
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
\"dnf -y install git clang sqlite-devel boost-devel >/dev/null\",
\"mkdir -p /opt/snake\",
\"if [ ! -d /opt/snake/repo ]; then echo Missing /opt/snake/repo; exit 1; fi\",
\"cd /opt/snake/repo\",
\"git fetch --all --prune\",
\"git checkout ${APP_REF}\",
\"git pull --ff-only origin ${APP_REF}\",
\"if [ -f /opt/snake/repo/index.html ]; then mkdir -p /var/www/snake; cp -f /opt/snake/repo/index.html /var/www/snake/index.html; fi\",
\"if [ -f /var/www/snake/index.html ]; then sed -i 's|const API = \\\"http://127.0.0.1:8080\\\";|const API = window.location.origin;|' /var/www/snake/index.html || true; fi\",
\"clang++ -std=c++17 -O2 -pthread ${BUILD_TARGET} -o /opt/snake/snake_server -lboost_system -lsqlite3\",
\"if ! command -v caddy >/dev/null 2>&1; then dnf -y install dnf-plugins-core ca-certificates >/dev/null; dnf config-manager --add-repo https://dl.cloudsmith.io/public/caddy/stable/rpm.repo >/dev/null; rpm --import https://dl.cloudsmith.io/public/caddy/stable/gpg.key >/dev/null 2>&1 || true; dnf -y install caddy >/dev/null; fi\",
\"cat > /etc/caddy/Caddyfile <<'EOF_CADDY'\",
\"${DOMAIN_NAME} {\",
\"  encode zstd gzip\",
\"  @api path /auth/* /me/* /snakes/* /game/*\",
\"  handle @api {\",
\"    reverse_proxy 127.0.0.1:${APP_PORT}\",
\"  }\",
\"  root * /var/www/snake\",
\"  file_server\",
\"}\",
\"EOF_CADDY\",
\"systemctl daemon-reload || true\",
\"systemctl restart snake\",
\"systemctl is-active snake\",
\"systemctl enable --now caddy\",
\"systemctl is-active caddy\"
]" \
    --query 'Command.CommandId' \
    --output text
)"

echo "SSM command id: ${COMMAND_ID}"

if ! aws ssm wait command-executed --region "$REGION" --command-id "$COMMAND_ID" --instance-id "$INSTANCE_ID"; then
  echo "SSM command failed. Fetching command output..."
  aws ssm list-command-invocations \
    --region "$REGION" \
    --command-id "$COMMAND_ID" \
    --details \
    --query 'CommandInvocations[0].CommandPlugins[0].Output' \
    --output text || true
  exit 4
fi

STATUS="$(
  aws ssm list-command-invocations \
    --region "$REGION" \
    --command-id "$COMMAND_ID" \
    --details \
    --query 'CommandInvocations[0].Status' \
    --output text
)"

echo "Deploy status: ${STATUS}"

if [[ "$STATUS" != "Success" ]]; then
  aws ssm list-command-invocations \
    --region "$REGION" \
    --command-id "$COMMAND_ID" \
    --details \
    --query 'CommandInvocations[0].CommandPlugins[0].Output' \
    --output text || true
  exit 4
fi

echo "Deploy completed."
