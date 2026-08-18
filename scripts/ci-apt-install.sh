#!/usr/bin/env bash
# Install packages on GitHub ubuntu-24.04 runners without letting
# azure.archive.ubuntu.com eat the job timeout. Those runners pin that
# mirror through /etc/apt/apt-mirrors.txt; it periodically stalls mid-update
# while sibling jobs on the same matrix finish in a few minutes.

set -euo pipefail

if (( $# == 0 )); then
  echo "usage: $0 <apt-packages...>" >&2
  exit 2
fi

rewrite_azure_mirror() {
  local path="$1"
  if [[ -f "${path}" ]]; then
    sudo sed -i 's|azure.archive.ubuntu.com|archive.ubuntu.com|g' "${path}"
  fi
}

shopt -s nullglob
rewrite_azure_mirror /etc/apt/sources.list
rewrite_azure_mirror /etc/apt/apt-mirrors.txt
for path in /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  rewrite_azure_mirror "${path}"
done

sudo apt-get update \
  -o Acquire::Retries=5 \
  -o Acquire::http::Timeout=20 \
  -o Acquire::https::Timeout=20 \
  -o Acquire::ftp::Timeout=20
sudo apt-get install -y "$@"
