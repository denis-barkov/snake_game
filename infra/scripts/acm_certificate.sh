#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-}"
ENV_NAME="${ENV:-${2:-}}"
DOMAIN_NAME="${DOMAIN:-${3:-}}"
WILDCARD_FLAG="${WILDCARD:-}"
REGION="${AWS_REGION:-us-east-1}"
PROJECT_TAG_FIXED="snake-game"
VALIDATION_TIMEOUT_SECONDS=360
VALIDATION_POLL_SECONDS=20

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

find_public_hosted_zone_id_for_domain() {
  local domain="$1"
  local domain_fqdn="${domain%.}."
  local best_zone_name=""
  local best_zone_id=""
  local output
  output="$("${AWS_BASE[@]}" route53 list-hosted-zones --query 'HostedZones[?Config.PrivateZone==`false`].[Name,Id]' --output text)"
  while IFS=$'\t' read -r zone_name zone_id; do
    [[ -z "${zone_name:-}" || -z "${zone_id:-}" ]] && continue
    local zn="${zone_name%.}"
    local dn="${domain_fqdn%.}"
    if [[ "${dn}" == "${zn}" || "${dn}" == *".${zn}" ]]; then
      if [[ ${#zn} -gt ${#best_zone_name} ]]; then
        best_zone_name="${zn}"
        best_zone_id="${zone_id##*/}"
      fi
    fi
  done <<< "${output}"
  if [[ -z "${best_zone_id}" ]]; then
    return 1
  fi
  echo "${best_zone_id}"
}

fetch_validation_records_tsv() {
  local arn="$1"
  "${AWS_BASE[@]}" acm describe-certificate \
    --certificate-arn "${arn}" \
    --query 'Certificate.DomainValidationOptions[?ResourceRecord!=null].ResourceRecord.[Name,Type,Value]' \
    --output text
}

upsert_route53_record() {
  local zone_id="$1"
  local record_name="$2"
  local record_type="$3"
  local record_value="$4"

  "${AWS_BASE[@]}" route53 change-resource-record-sets \
    --hosted-zone-id "${zone_id}" \
    --change-batch "{
      \"Comment\": \"ACM validation record managed by snake SSL automation\",
      \"Changes\": [{
        \"Action\": \"UPSERT\",
        \"ResourceRecordSet\": {
          \"Name\": \"${record_name}\",
          \"Type\": \"${record_type}\",
          \"TTL\": 60,
          \"ResourceRecords\": [{\"Value\": \"${record_value}\"}]
        }
      }]
    }" >/dev/null
}

wait_for_issued_status() {
  local arn="$1"
  local elapsed=0
  local status="UNKNOWN"
  while (( elapsed <= VALIDATION_TIMEOUT_SECONDS )); do
    status="$(print_cert_status "${arn}" || echo UNKNOWN)"
    echo "Waiting for ISSUED... current_status=${status} elapsed=${elapsed}s"
    if [[ "${status}" == "ISSUED" ]]; then
      echo "Certificate issued successfully."
      echo "certificate_arn=${arn}"
      return 0
    fi
    sleep "${VALIDATION_POLL_SECONDS}"
    elapsed=$((elapsed + VALIDATION_POLL_SECONDS))
  done
  echo "Certificate validation timeout. Current status: ${status}"
  echo "Check DNS propagation or ACM validation record."
  return 4
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
      echo "Certificate already exists: ${EXISTING_ARN}"
      echo "Status: ${STATUS}"
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

    echo "Resolving hosted zone for domain: ${DOMAIN_NAME}"
    ZONE_ID="$(find_public_hosted_zone_id_for_domain "${DOMAIN_NAME}")" || {
      echo "Failed to locate public Route53 hosted zone for domain '${DOMAIN_NAME}'."
      echo "Create validation CNAME manually with these ACM values:"
      print_dns_validation_records "${CERT_ARN}"
      exit 5
    }
    echo "Using hosted zone id: ${ZONE_ID}"

    # ACM may need a short delay before ResourceRecord fields are populated.
    records_tsv=""
    for _ in $(seq 1 6); do
      records_tsv="$(fetch_validation_records_tsv "${CERT_ARN}" || true)"
      if [[ -n "${records_tsv}" && "${records_tsv}" != "None" ]]; then
        break
      fi
      sleep 5
    done
    if [[ -z "${records_tsv}" || "${records_tsv}" == "None" ]]; then
      echo "Could not retrieve ACM DNS validation record yet."
      echo "Try again in a few seconds and check ACM console."
      exit 6
    fi

    echo "Upserting Route53 validation records:"
    # Keep portable dedupe logic for macOS bash 3.x (no associative arrays).
    seen_records_blob=""
    while IFS=$'\t' read -r rr_name rr_type rr_value; do
      [[ -z "${rr_name:-}" || -z "${rr_type:-}" || -z "${rr_value:-}" ]] && continue
      key="${rr_name}|${rr_type}|${rr_value}"
      if printf '%s\n' "${seen_records_blob}" | grep -Fqx "${key}"; then
        continue
      fi
      seen_records_blob+="${key}"$'\n'
      echo "  ${rr_name} ${rr_type} ${rr_value}"
      upsert_route53_record "${ZONE_ID}" "${rr_name}" "${rr_type}" "${rr_value}"
    done <<< "${records_tsv}"

    wait_for_issued_status "${CERT_ARN}"
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
