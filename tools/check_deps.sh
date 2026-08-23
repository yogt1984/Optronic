#!/usr/bin/env bash
# Dependency rule (SPEC-02 §1): third-party headers stay inside the module that owns them.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
rc=0
check() {  # pattern, allowed-dir, description
  local hits
  hits=$(git ls-files 'framework/*' 'modules/*' 'tools/*' 'tests/*' | grep -E '\.(cpp|hpp|h)$' | grep -v "^$2/" | xargs -r grep -ln -E "$1" || true)
  if [[ -n "$hits" ]]; then
    echo "dependency rule violated: $3"; echo "$hits" | sed 's/^/  /'; rc=1
  fi
}
check '#include[[:space:]]*<gst/'          'modules/video'     'GStreamer headers only in modules/video'
check '#include[[:space:]]*<mosquitto'     'modules/telemetry' 'mosquitto headers only in modules/telemetry'
check '#include[[:space:]]*<opencv2/'      'modules/detect'    'OpenCV headers only in modules/detect'
check '\bvolatile\b'                       'framework/hal'     'volatile only in framework/hal'
[[ $rc -eq 0 ]] && echo "dependency rules: ok"
exit $rc
