#!/usr/bin/env bash
#
# Copyright (c) 2026 Nutanix Inc. All rights reserved.
#
# Local developer workflow only: regenerates vendored C++ gRPC/protobuf stubs
# from the repository's local vdisk.proto into generated/.
#
# Run this script locally whenever updating .proto files, then commit the
# updated files in generated/ before pushing.
#
# Requires protoc and grpc_cpp_plugin on PATH (e.g. `brew install grpc`).
#
# Usage:
#   tools/regen_proto.sh
#   tools/regen_proto.sh --update-checksum

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

proto_file="${repo_root}/vdisk.proto"
out_dir="${repo_root}/generated"
checksum_file="${out_dir}/vdisk.proto.sha256"
update_checksum=false

if [[ "${1:-}" == "--update-checksum" ]]; then
  update_checksum=true
elif [[ $# -gt 0 ]]; then
  echo "error: unknown argument: $1" >&2
  echo "usage: tools/regen_proto.sh [--update-checksum]" >&2
  exit 1
fi

if [[ ! -f "${proto_file}" ]]; then
  echo "error: ${proto_file} not found" >&2
  exit 1
fi

plugin="$(command -v grpc_cpp_plugin)"
if [[ -z "${plugin}" ]]; then
  echo "error: grpc_cpp_plugin not found on PATH" >&2
  exit 1
fi

protoc_bin="$(command -v protoc)"
if [[ -z "${protoc_bin}" ]]; then
  echo "error: protoc not found on PATH" >&2
  exit 1
fi

mkdir -p "${out_dir}"

actual_sha="$(shasum -a 256 "${proto_file}" | awk '{print $1}')"
if [[ -f "${checksum_file}" ]]; then
  expected_sha="$(awk '{print $1}' "${checksum_file}")"
  if [[ "${actual_sha}" != "${expected_sha}" && "${update_checksum}" != true ]]; then
    echo "error: ${proto_file} checksum mismatch" >&2
    echo "  expected: ${expected_sha}" >&2
    echo "  actual:   ${actual_sha}" >&2
    echo "Run tools/regen_proto.sh --update-checksum when intentionally changing the proto." >&2
    exit 1
  fi
fi

"${protoc_bin}" \
  --proto_path="${repo_root}" \
  --cpp_out="${out_dir}" \
  --grpc_out="${out_dir}" \
  --plugin=protoc-gen-grpc="${plugin}" \
  "${proto_file}"

if [[ "${update_checksum}" == true || ! -f "${checksum_file}" ]]; then
  printf "%s  vdisk.proto\n" "${actual_sha}" > "${checksum_file}"
  echo "Updated checksum: ${checksum_file}"
fi

echo "Generated stubs in ${out_dir}"
