#!/bin/zsh
# Every wire format this addon can send, against every receive colour format
# the SDK offers, decoded through the real GPU pipeline and measured against
# SMPTE ECR 1-1978 75% bars.
#
# The send direction is covered by the sweep test and by GStreamer's ndisrc.
# This is the receive direction, and it is the only way to see what the SDK
# actually hands over for a given pair -- which is rarely what the sender put
# on the wire.
#
# Usage: ndi-matrix.sh <build-dir> <libndi.so>

set -u
BUILD="${1:?build dir}"
NDILIB="${2:?path to libndi.so}"
SWEEP="$BUILD/score_addon_ndi_format_sweep_test"
ALPHA="$BUILD/score_addon_ndi_alpha_sender"
PROBE="$BUILD/score_addon_ndi_live_decode_probe"
HOST=$(hostname | tr 'a-z' 'A-Z')

# NDIlib_recv_color_format_e. 100/101 also force fields regardless of
# allow_video_fields, which is why an interlaced source behaves differently
# under them.
typeset -A MODES
MODES=(
  0   "BGRX_BGRA"
  1   "UYVY_BGRA"
  2   "RGBX_RGBA"
  3   "UYVY_RGBA"
  100 "fastest"
  101 "best"
)
MODE_ORDER=(0 1 2 3 100 101)

# Formats the output encodes, plus the two alpha layouts only the dedicated
# sender produces.
SEND_FORMATS=(RGBA RGBX BGRA BGRX UYVY P216 NV12 I420 YV12)
ALPHA_FORMATS=(UYVA PA16)

kill_senders() {
  for p in $(pgrep -f "serve=|format=UYVA|format=PA16" 2>/dev/null); do
    c=$(ps -o comm= -p $p 2>/dev/null)
    case "$c" in *zsh*|*sh) ;; *) kill $p 2>/dev/null ;; esac
  done
  sleep 1
}

run_one() {  # name mode_id extra_flags...
  local src="$1" m="$2"; shift 2
  local out
  out=$(timeout 180 "$PROBE" "$NDILIB" --mode=$m --filter="$src" "$@" 2>/dev/null \
        | grep '^RESULT')
  if [[ -z "$out" ]]; then
    printf "%-10s" "-"
    return
  fi
  local fourcc err fail
  fourcc=$(sed -n 's/.*fourcc=\([A-Za-z0-9()]*\).*/\1/p' <<< "$out")
  err=$(sed -n 's/.*err=\([0-9.]*\).*/\1/p' <<< "$out")
  fail=$(sed -n 's/.*fail=\([0-9]*\).*/\1/p' <<< "$out")
  if [[ "$fail" != "0" ]]; then
    printf "%-10s" "${fourcc}!"
  else
    printf "%-10s" "${fourcc}/${err}"
  fi
}

echo "NDI receive matrix -- sender format down, receive mode across."
echo "Cell: FourCC delivered / mean |RGB error| vs 75% SMPTE. '!' = a check failed."
echo
printf "%-7s" "send"
for m in $MODE_ORDER; do printf "%-10s" "${MODES[$m]}"; done
echo

kill_senders
for f in $SEND_FORMATS; do
  "$SWEEP" "$NDILIB" --phases= --serve=$f >/dev/null 2>&1 &
  SV=$!
  sleep 6
  printf "%-7s" "$f"
  for m in $MODE_ORDER; do run_one "score-$f" $m; done
  echo
  kill $SV 2>/dev/null; wait $SV 2>/dev/null
done

for f in $ALPHA_FORMATS; do
  "$ALPHA" "$NDILIB" --format=$f >/dev/null 2>&1 &
  SV=$!
  sleep 6
  printf "%-7s" "$f"
  for m in $MODE_ORDER; do run_one "alpha-$f" $m --alpha-ramp; done
  echo
  kill $SV 2>/dev/null; wait $SV 2>/dev/null
done
kill_senders
