#!/usr/bin/env bash
set -euo pipefail

PROFILE="${AWS_PROFILE:-business}"
REGION="${AWS_REGION:-us-east-1}"
PROJECT="${PROJECT_TAG:-snake-game}"
ENVIRONMENT="${ENVIRONMENT_TAG:-prod}"
PREFIX="${PROJECT}-${ENVIRONMENT}"
DOMAIN_NAME="${DOMAIN_NAME:-terrariumsnake.com}"

echo "This will delete AWS resources tagged project=${PROJECT}, environment=${ENVIRONMENT} in ${REGION}."
read -r -p "Type '${PREFIX}' to continue: " ACK
if [[ "${ACK}" != "${PREFIX}" ]]; then
  echo "Aborted."
  exit 1
fi

aws_cli() { aws --profile "${PROFILE}" --region "${REGION}" "$@"; }

echo "[1/10] Deleting Auto Scaling Group ${PREFIX}-asg (force)..."
if aws_cli autoscaling describe-auto-scaling-groups --auto-scaling-group-names "${PREFIX}-asg" \
  --query 'AutoScalingGroups[0].AutoScalingGroupName' --output text 2>/dev/null | grep -q "${PREFIX}-asg"; then
  aws_cli autoscaling delete-auto-scaling-group --auto-scaling-group-name "${PREFIX}-asg" --force-delete || true
fi

echo "[2/10] Terminating tagged EC2 instances..."
INSTANCE_IDS=$(aws_cli ec2 describe-instances \
  --filters \
    "Name=tag:project,Values=${PROJECT}" \
    "Name=tag:environment,Values=${ENVIRONMENT}" \
    "Name=instance-state-name,Values=pending,running,stopping,stopped" \
  --query 'Reservations[].Instances[].InstanceId' --output text)
if [[ -n "${INSTANCE_IDS}" && "${INSTANCE_IDS}" != "None" ]]; then
  aws_cli ec2 terminate-instances --instance-ids ${INSTANCE_IDS} >/dev/null || true
  aws_cli ec2 wait instance-terminated --instance-ids ${INSTANCE_IDS} || true
fi

echo "[3/10] Deleting launch templates tagged ${PROJECT}/${ENVIRONMENT}..."
LT_IDS=$(aws_cli ec2 describe-launch-templates \
  --query "LaunchTemplates[?Tags[?Key=='project'&&Value=='${PROJECT}'] && Tags[?Key=='environment'&&Value=='${ENVIRONMENT}']].LaunchTemplateId" \
  --output text)
for lt in ${LT_IDS:-}; do
  aws_cli ec2 delete-launch-template --launch-template-id "${lt}" || true
done

echo "[4/10] Deleting DynamoDB tables..."
for table in \
  "${PREFIX}-users" \
  "${PREFIX}-snakes" \
  "${PREFIX}-world_chunks" \
  "${PREFIX}-snake_events" \
  "${PREFIX}-settings" \
  "${PREFIX}-economy_params" \
  "${PREFIX}-economy_period" \
  "${PREFIX}-economy_period_user"; do
  aws_cli dynamodb delete-table --table-name "${table}" >/dev/null 2>&1 || true
done

echo "[5/10] Deleting CloudWatch log groups with prefix /${PROJECT}/${ENVIRONMENT}..."
LOG_GROUPS=$(aws_cli logs describe-log-groups --log-group-name-prefix "/${PROJECT}/${ENVIRONMENT}" \
  --query 'logGroups[].logGroupName' --output text)
for lg in ${LOG_GROUPS:-}; do
  aws_cli logs delete-log-group --log-group-name "${lg}" || true
done

echo "[6/10] Deleting SSM parameter /${PROJECT}/${ENVIRONMENT}/eip_allocation_id..."
aws_cli ssm delete-parameter --name "/${PROJECT}/${ENVIRONMENT}/eip_allocation_id" >/dev/null 2>&1 || true

echo "[7/10] Deleting Route53 A record for ${DOMAIN_NAME} (if present)..."
ZONE_ID=$(aws --profile "${PROFILE}" route53 list-hosted-zones-by-name \
  --dns-name "${DOMAIN_NAME}" --query 'HostedZones[0].Id' --output text 2>/dev/null || true)
if [[ -n "${ZONE_ID}" && "${ZONE_ID}" != "None" ]]; then
  REC_JSON=$(aws --profile "${PROFILE}" route53 list-resource-record-sets --hosted-zone-id "${ZONE_ID}" \
    --query "ResourceRecordSets[?Name=='${DOMAIN_NAME}.' && Type=='A'] | [0]" --output json)
  if [[ "${REC_JSON}" != "null" ]]; then
    aws --profile "${PROFILE}" route53 change-resource-record-sets --hosted-zone-id "${ZONE_ID}" --change-batch "{
      \"Changes\": [{\"Action\": \"DELETE\", \"ResourceRecordSet\": ${REC_JSON}}]
    }" >/dev/null || true
  fi
fi

echo "[8/10] Releasing Elastic IPs tagged ${PROJECT}/${ENVIRONMENT}..."
EIP_ALLOCS=$(aws_cli ec2 describe-addresses \
  --query "Addresses[?Tags[?Key=='project'&&Value=='${PROJECT}'] && Tags[?Key=='environment'&&Value=='${ENVIRONMENT}']].AllocationId" \
  --output text)
for alloc in ${EIP_ALLOCS:-}; do
  aws_cli ec2 release-address --allocation-id "${alloc}" || true
done

echo "[9/10] Deleting IAM instance profiles/roles with prefix ${PREFIX} (best effort)..."
for ip in $(aws --profile "${PROFILE}" iam list-instance-profiles \
  --query "InstanceProfiles[?starts_with(InstanceProfileName, '${PREFIX}')].InstanceProfileName" --output text); do
  ROLE_NAMES=$(aws --profile "${PROFILE}" iam get-instance-profile --instance-profile-name "${ip}" \
    --query 'InstanceProfile.Roles[].RoleName' --output text)
  for role in ${ROLE_NAMES:-}; do
    aws --profile "${PROFILE}" iam remove-role-from-instance-profile --instance-profile-name "${ip}" --role-name "${role}" || true
  done
  aws --profile "${PROFILE}" iam delete-instance-profile --instance-profile-name "${ip}" || true
done
for role in $(aws --profile "${PROFILE}" iam list-roles \
  --query "Roles[?starts_with(RoleName, '${PREFIX}')].RoleName" --output text); do
  for pol in $(aws --profile "${PROFILE}" iam list-attached-role-policies --role-name "${role}" \
    --query 'AttachedPolicies[].PolicyArn' --output text); do
    aws --profile "${PROFILE}" iam detach-role-policy --role-name "${role}" --policy-arn "${pol}" || true
  done
  for pol in $(aws --profile "${PROFILE}" iam list-role-policies --role-name "${role}" --query 'PolicyNames[]' --output text); do
    aws --profile "${PROFILE}" iam delete-role-policy --role-name "${role}" --policy-name "${pol}" || true
  done
  aws --profile "${PROFILE}" iam delete-role --role-name "${role}" || true
done

echo "[10/10] Deleting tagged VPCs and common dependencies..."
VPCS=$(aws_cli ec2 describe-vpcs \
  --filters "Name=tag:project,Values=${PROJECT}" "Name=tag:environment,Values=${ENVIRONMENT}" \
  --query 'Vpcs[].VpcId' --output text)
for vpc in ${VPCS:-}; do
  for nat in $(aws_cli ec2 describe-nat-gateways --filter "Name=vpc-id,Values=${vpc}" \
    --query 'NatGateways[].NatGatewayId' --output text); do
    aws_cli ec2 delete-nat-gateway --nat-gateway-id "${nat}" || true
  done

  for igw in $(aws_cli ec2 describe-internet-gateways \
    --filters "Name=attachment.vpc-id,Values=${vpc}" \
    --query 'InternetGateways[].InternetGatewayId' --output text); do
    aws_cli ec2 detach-internet-gateway --internet-gateway-id "${igw}" --vpc-id "${vpc}" || true
    aws_cli ec2 delete-internet-gateway --internet-gateway-id "${igw}" || true
  done

  for rtb in $(aws_cli ec2 describe-route-tables --filters "Name=vpc-id,Values=${vpc}" \
    --query 'RouteTables[?Associations[?Main!=`true`]].RouteTableId' --output text); do
    for assoc in $(aws_cli ec2 describe-route-tables --route-table-ids "${rtb}" \
      --query 'RouteTables[].Associations[].RouteTableAssociationId' --output text); do
      aws_cli ec2 disassociate-route-table --association-id "${assoc}" || true
    done
    aws_cli ec2 delete-route-table --route-table-id "${rtb}" || true
  done

  for subnet in $(aws_cli ec2 describe-subnets --filters "Name=vpc-id,Values=${vpc}" \
    --query 'Subnets[].SubnetId' --output text); do
    aws_cli ec2 delete-subnet --subnet-id "${subnet}" || true
  done

  for sg in $(aws_cli ec2 describe-security-groups --filters "Name=vpc-id,Values=${vpc}" \
    --query "SecurityGroups[?GroupName!='default'].GroupId" --output text); do
    aws_cli ec2 delete-security-group --group-id "${sg}" || true
  done

  aws_cli ec2 delete-vpc --vpc-id "${vpc}" || true
done

echo "Cleanup complete. Remaining tagged resources check:"
echo "aws --profile ${PROFILE} resourcegroupstaggingapi get-resources --tag-filters Key=project,Values=${PROJECT} Key=environment,Values=${ENVIRONMENT} --region ${REGION} --query 'ResourceTagMappingList[].ResourceARN' --output text"
