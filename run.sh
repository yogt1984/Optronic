#!/usr/bin/env bash
# Run the demo: check the camera, then stream it through the detector.
#
#   ./run.sh
#
# Expects ./install.sh to have been run once. Ctrl-C stops everything.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

if ! docker image inspect optronic/debian >/dev/null 2>&1; then
  echo "container image missing - run ./install.sh first" >&2
  exit 1
fi

printf '\n\033[1mCameras\033[0m\n'
# --probe grabs a real frame from each: a device node existing proves nothing,
# and a camera held by another process looks fine until it is opened.
tools/cameras.sh --probe

printf '\n\033[1mStarting the detector - Ctrl-C to stop\033[0m\n'
exec tools/demo.sh detect
