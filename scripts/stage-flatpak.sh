#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
cr_build_dir="${1:-build-steamrt64}"
python3 scripts/check-linux64-abi.py "$cr_build_dir/cloud_redirect.so" --cli "$cr_build_dir/cloud_redirect_cli"
install -m755 "$cr_build_dir/cloud_redirect.so" flatpak/cloud_redirect.so
install -m755 "$cr_build_dir/cloud_redirect_cli" flatpak/cloud_redirect_cli
