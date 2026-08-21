#!/usr/bin/env bash
# Format or check (with --check) all C++ sources tracked by git.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
mode=${1:-}
# Legacy sources at the top level are left as they are until they are replaced.
files=$(git ls-files 'framework/*' 'modules/*' 'tools/*' 'tests/*' | grep -E '\.(cpp|hpp|h|cc)$' || true)
if [[ -z "$files" ]]; then echo "format: nothing to check"; exit 0; fi
if [[ "$mode" == "--check" ]]; then
  clang-format --dry-run --Werror $files
else
  clang-format -i $files
fi
