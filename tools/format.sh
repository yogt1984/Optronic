#!/usr/bin/env bash
# Format or check (with --check) all C++ sources tracked by git.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
mode=${1:-}
files=$(git ls-files '*.cpp' '*.hpp' '*.h' '*.cc')
if [[ "$mode" == "--check" ]]; then
  clang-format --dry-run --Werror $files
else
  clang-format -i $files
fi
