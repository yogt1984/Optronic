#!/usr/bin/env bash
# Find a usable camera.
#
#   tools/cameras.sh          list what is attached and which nodes work
#   tools/cameras.sh --pick   print the best device path and nothing else
#   tools/cameras.sh --probe  same as the list, but actually grab a frame
#                             from each candidate instead of trusting v4l2
#
# A USB camera claims several /dev/video* nodes and only some of them capture:
# the rest carry metadata. Which number a camera gets depends on enumeration
# order, so it changes when something is unplugged. Picking /dev/video0 and
# hoping is how a demo fails in front of an audience.
#
# Preference order: an external camera over a built-in one - if someone plugged
# a camera in, that is the one they meant.
set -euo pipefail

mode=${1:-list}

card_of() { v4l2-ctl -d "$1" --info 2>/dev/null | awk -F': ' '/Card type/{print $2; exit}'; }
bus_of() { v4l2-ctl -d "$1" --info 2>/dev/null | awk -F': ' '/Bus info/{print $2; exit}'; }

captures() { # device -> yes if it can actually capture video, not just metadata
  v4l2-ctl -d "$1" --list-formats 2>/dev/null | grep -q "\[0\]"
}

best_size() { # largest advertised resolution, for the summary line
  v4l2-ctl -d "$1" --list-formats-ext 2>/dev/null |
    grep -oE '[0-9]+x[0-9]+' | sort -t x -k1 -n | tail -1
}

# A frame really arriving is the only proof that matters: permissions, a
# camera claimed by another process and a covered lens all pass every check
# above and fail here.
delivers() {
  timeout 12 gst-launch-1.0 -q v4l2src device="$1" num-buffers=3 ! fakesink >/dev/null 2>&1
}

is_external() { # anything that does not call itself integrated or built-in
  local c
  c=$(card_of "$1" | tr '[:upper:]' '[:lower:]')
  [[ -n $c && $c != *integrated* && $c != *built-in* ]]
}

candidates=()
for d in /dev/video*; do
  [[ -e $d ]] || continue
  captures "$d" || continue
  candidates+=("$d")
done

if [[ ${#candidates[@]} -eq 0 ]]; then
  echo "no capture-capable /dev/video* found." >&2
  echo "plug the camera in, then: lsusb | grep -i cam" >&2
  exit 1
fi

# External first, then by device number, so the pick is deterministic.
ranked=()
for d in "${candidates[@]}"; do is_external "$d" && ranked+=("$d"); done
for d in "${candidates[@]}"; do is_external "$d" || ranked+=("$d"); done

if [[ $mode == --pick ]]; then
  for d in "${ranked[@]}"; do
    delivers "$d" && { echo "$d"; exit 0; }
  done
  echo "capture nodes exist but none delivered a frame." >&2
  echo "in use by another process, or no permission - try: tools/cameras.sh --probe" >&2
  exit 1
fi

printf '%-14s %-28s %-10s %-12s %s\n' DEVICE NAME KIND MAX WORKS
for d in "${ranked[@]}"; do
  kind=$(is_external "$d" && echo external || echo built-in)
  works="-"
  [[ $mode == --probe ]] && { delivers "$d" && works=yes || works=NO; }
  printf '%-14s %-28s %-10s %-12s %s\n' "$d" "$(card_of "$d")" "$kind" "$(best_size "$d")" "$works"
done

echo
echo "bus:"
for d in "${ranked[@]}"; do printf '  %-14s %s\n' "$d" "$(bus_of "$d")"; done
[[ $mode == --probe ]] || echo $'\nadd --probe to actually grab a frame from each'
