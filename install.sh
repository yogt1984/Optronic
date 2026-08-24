#!/usr/bin/env bash
# One-time setup on a Debian or Ubuntu machine.
#
#   ./install.sh
#
# Installs what has to live on the host - Docker, and the GStreamer pieces that
# decode and display the stream - then builds the container image, fetches the
# detection model and warms the build. Everything else stays inside the
# container, which is the point: the host needs a decoder and a display, not a
# toolchain.
#
# Safe to run twice. Each step checks before it acts.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }
die()  { printf '  \033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. platform
say "1/5  Checking the machine"

[[ -r /etc/os-release ]] || die "no /etc/os-release - this script is for Debian and Ubuntu"
. /etc/os-release
case "${ID:-}${ID_LIKE:-}" in
  *debian*|*ubuntu*) ok "${PRETTY_NAME:-$ID}" ;;
  *) die "this is ${PRETTY_NAME:-$ID}; the script only knows apt" ;;
esac
command -v sudo >/dev/null || die "sudo not found"

# ------------------------------------------------------------- 2. host packages
say "2/5  Host packages"

# Only what genuinely cannot live in the container: Docker itself, the decoder
# and sink that put the stream on screen, and v4l-utils to enumerate cameras.
packages=(docker.io curl v4l-utils
          gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
          gstreamer1.0-plugins-bad gstreamer1.0-libav)

missing=()
for p in "${packages[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done

if [[ ${#missing[@]} -eq 0 ]]; then
  ok "all present"
else
  echo "  installing: ${missing[*]}"
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends "${missing[@]}"
  ok "installed"
fi

# --------------------------------------------------------------- 3. docker access
say "3/5  Docker"

sudo systemctl enable --now docker >/dev/null 2>&1 || true

if docker info >/dev/null 2>&1; then
  ok "usable without sudo"
else
  if ! id -nG "$USER" | grep -qw docker; then
    sudo usermod -aG docker "$USER"
    warn "added $USER to the docker group - log out and back in, then re-run this"
    warn "or continue in this shell with: newgrp docker"
    exit 1
  fi
  die "docker is installed but not responding - try: sudo systemctl status docker"
fi

# ---------------------------------------------------------------- 4. the image
say "4/5  Build container (a few minutes the first time)"

if docker image inspect optronic/debian:cv >/dev/null 2>&1; then
  ok "optronic/debian:cv already built"
else
  DOCKER_BUILDKIT=1 docker build -f docker/Dockerfile.debian -t optronic/debian:cv .
  ok "built"
fi
# The demo scripts look for optronic/debian; :cv is the same image with OpenCV.
docker tag optronic/debian:cv optronic/debian:latest
ok "tagged optronic/debian:latest"

# ------------------------------------------------------------ 5. model + warm
say "5/5  Detection model and a warm build"

tools/get_model.sh
OPTRONIC_DETECT_IMAGE=optronic/debian:cv tools/demo.sh warm

say "Done"
echo "  ./run.sh          camera check, then the live detector demo"
echo "  tools/test.sh     the test suite"
echo
