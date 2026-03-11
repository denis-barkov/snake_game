#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-}"
ENV_NAME="${ENV:-${2:-}}"
DOMAIN_NAME="${DOMAIN:-${3:-}}"
WILDCARD_FLAG="${WILDCARD:-}"
REGION="${AWS_REGION:-us-east-1}"
PROJECT_TAG_FIXED="snake-game"

if [[ -z "${ACTION}" ]]; then
  echo "Usage: $0 <check|create|delete> ENV=<environment> DOMAIN=<domain> [WILDCARD=--wildcard|true]"
  exit 1
fi
if [[ -z "${ENV_NAME}" || -z "${DOMAIN_NAME}" ]]; then
  echo "ENV and DOMAIN are required."
  exit 1
fi

if [[ "${WILDCARD_FLAG}" == "--wildcard" || "${WILDCARD_FLAG}" == "true" || "${WILDCARD_FLAG}" == "1" ]]; then
  USE_WILDCARD=true
else
  USE_WILDCARD=false
fi

AWS_BASE=(aws --region "${REGION}")
if [[ -n "${AWS_PROFILE:-}" ]]; then
  AWS_BASE+=(--profile "${AWS_PROFILE}")
fi

find_cert_arn_by_tags() {
  "${AWS_BASE[@]}" resourcegroupstaggingapi get-resources \
    --resource-type-filters acm:certificate \
    --tag-filters \
      "Key=project,Values=${PROJECT_TAG_FIXED}" \
      "Key=environment,Values=${ENV_NAME}" \
      "Key=domain,Values=${DOMAIN_NAME}" \
    --query 'ResourceTagMappingList[0].ResourceARN' \
    --output text 2>/dev/null || true
}

print_cert_status() {
  local arn="$1"
  "${AWS_BASE[@]}" acm describe-certificate \
    --certificate-arn "${arn}" \
    --query 'Certificate.Status' \
    --output text
}

print_dns_validation_records() {
  local arn="$1"
  "${AWS_BASE[@]}" acm describe-certificate \
    --certificate-arn "${arn}" \
    --query 'Certificate.DomainValidationOptions[].ResourceRecord.[Name,Type,Value]' \
    --output table || true
}

case "${ACTION}" in
  check)
    EXISTING_ARN="$(find_cert_arn_by_tags)"
    if [[ -z "${EXISTING_ARN}" || "${EXISTING_ARN}" == "None" ]]; then
      echo "No ISSUED certificate found for environment='${ENV_NAME}' domain='${DOMAIN_NAME}'."
      echo "Create it first with:"
      echo "  make ssl-cert-create ENV=${ENV_NAME} DOMAIN=${DOMAIN_NAME}"
      exit 2
    fi
    STATUS="$(print_cert_status "${EXISTING_ARN}")"
    if [[ "${STATUS}" != "ISSUED" ]]; then
      echo "Certificate found but status is '${STATUS}', expected ISSUED."
      exit 3
    fi
    echo "Using existing ACM certificate for ${DOMAIN_NAME} (${ENV_NAME})"
    echo "certificate_arn=${EXISTING_ARN}"
    ;;

  create)
    EXISTING_ARN="$(find_cert_arn_by_tags)"
    if [[ -n "${EXISTING_ARN}" && "${EXISTING_ARN}" != "None" ]]; then
      STATUS="$(print_cert_status "${EXISTING_ARN}")"
      echo "Certificate already exists for this environment and domain."
      echo "certificate_arn=${EXISTING_ARN}"
      echo "status=${STATUS}"
      exit 0
    fi

    REQUEST_ARGS=(
      acm request-certificate
      --domain-name "${DOMAIN_NAME}"
      --validation-method DNS
      --tags
      "Key=project,Value=${PROJECT_TAG_FIXED}"
      "Key=environment,Value=${ENV_NAME}"
      "Key=domain,Value=${DOMAIN_NAME}"
    )
    if [[ "${USE_WILDCARD}" == "true" ]]; then
      REQUEST_ARGS+=(--subject-alternative-names "*.${DOMAIN_NAME}")
    fi

    CERT_ARN="$("${AWS_BASE[@]}" "${REQUEST_ARGS[@]}" --query 'CertificateArn' --output text)"
    echo "Certificate requested."
    echo "certificate_arn=${CERT_ARN}"
    echo "DNS validation records:"
    print_dns_validation_records "${CERT_ARN}"
    echo "ACM certificates auto-renew. Keep DNS validation records in Route53 for renewal continuity."
    ;;

  delete)
    EXISTING_ARN="$(find_cert_arn_by_tags)"
    if [[ -z "${EXISTING_ARN}" || "${EXISTING_ARN}" == "None" ]]; then
      echo "No certificate found for this environment and domain."
      exit 0
    fi
    "${AWS_BASE[@]}" acm delete-certificate --certificate-arn "${EXISTING_ARN}"
    echo "Deleted certificate: ${EXISTING_ARN}"
    ;;

  *)
    echo "Unknown action: ${ACTION}"
    echo "Usage: $0 <check|create|delete> ENV=<environment> DOMAIN=<domain> [WILDCARD=--wildcard|true]"
    exit 1
    ;;
esac
