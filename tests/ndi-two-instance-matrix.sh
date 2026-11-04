#!/bin/zsh
# Every NDI output format against every NDI input receive format, between TWO
# score instances, measured on the picture that comes out of the second one.
#
#   sender instance:   tests/matrix/pattern.fs -> NDI output "<prefix>-<FMT>"
#                      in format FMT (the output's Format combo box)
#   receiver instance: NDI input on that source, with Receive format MODE (the
#                      input's Receive format combo box) -> passthrough.fs ->
#                      NDI output "<prefix>relay-<FMT>-<MODE>" in RGBA
#   MatrixReceiver:    reads the relay and compares it with the picture
#
# The relay is RGBA, so what is measured is everything between the sender's
# scene and the receiver's decoded texture, in 8-bit RGB.
#
# Usage: ndi-two-instance-matrix.sh [options]
#   --app DIR          directory holding ossia-score and plugins/ (required).
#                      plugins/ is also put first on LD_LIBRARY_PATH, so a
#                      directory that overrides some plugins with other builds
#                      really runs those.
#   --ndi PATH         libndi.so for the measuring receiver (default: the SDK 6
#                      runtime in /opt/sdk/ndi, else libndi.so.6)
#   --sapi API         sender graphics API: OpenGL | Vulkan (default OpenGL)
#   --rapi API         receiver graphics API (default: same as --sapi)
#   --size WxH         picture size (default 1920x1080)
#   --formats "..."    output formats (default: the whole combo box)
#   --modes "..."      receive formats: 8bit best (default: both)
#   --gfx "K=V ..."    extra [score_plugin_gfx] settings for both instances,
#                      e.g. "Samples=4"
#   --label TEXT       table title
#   --work DIR         profiles, logs and the built receiver (default: a
#                      directory under $TMPDIR)
#   --displays "A B"   X displays for the two instances (default: two private
#                      Xvfb servers started and stopped here)
#   --out FILE         append the markdown table to FILE as well
#                      (the frame each cell measured is kept in --work as
#                      relay-<sapi>-<rapi>-<format>-<mode>.ppm)
#   --rescore FILE     run nothing: rebuild the table from the raw RESULT
#                      lines a previous run left in FILE (its results.txt),
#                      for the given --sapi/--rapi, with the current verdicts
#
# Exit status: 0 when every cell passes, 1 otherwise.

set -u
SELF_DIR=${0:A:h}
ADDON_DIR=${SELF_DIR:h}

APP="" NDILIB="" SAPI=OpenGL RAPI="" SIZE=1920x1080
FORMATS="RGBA RGBX BGRA BGRX UYVY P216 NV12 I420 YV12"
MODES="8bit best" GFX="" LABEL="" WORK="" DISPLAYS="" OUT="" RESCORE=""
while (( $# )); do
  case "$1" in
    --app) APP=$2; shift 2;;
    --ndi) NDILIB=$2; shift 2;;
    --sapi) SAPI=$2; shift 2;;
    --rapi) RAPI=$2; shift 2;;
    --size) SIZE=$2; shift 2;;
    --formats) FORMATS=$2; shift 2;;
    --modes) MODES=$2; shift 2;;
    --gfx) GFX=$2; shift 2;;
    --label) LABEL=$2; shift 2;;
    --work) WORK=$2; shift 2;;
    --displays) DISPLAYS=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --rescore) RESCORE=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
[[ -n "$RESCORE" || -x "$APP/ossia-score" ]] \
  || { echo "--app must hold ossia-score" >&2; exit 2; }
[[ -n "$APP" ]] && APP=${APP:A}
RAPI=${RAPI:-$SAPI}
W=${SIZE%x*} H=${SIZE#*x}
if [[ -z "$NDILIB" ]]; then
  NDILIB=/opt/sdk/ndi/lib/x86_64-linux-gnu/libndi.so.6
  [[ -e $NDILIB ]] || NDILIB=libndi.so.6
fi
WORK=${WORK:-$(mktemp -d ${TMPDIR:-/tmp}/ndi-matrix.XXXXXX)}
mkdir -p $WORK
HOSTU=$(hostname | tr 'a-z' 'A-Z')
PREFIX="m$$"

if [[ -z "$RESCORE" ]]; then
# ------------------------------------------------------------------- receiver
RECV=$WORK/MatrixReceiver
if [[ ! -x $RECV || $SELF_DIR/matrix/MatrixReceiver.cpp -nt $RECV ]]; then
  c++ -O2 -std=c++20 -I$ADDON_DIR/3rdparty/NDI/include \
      $SELF_DIR/matrix/MatrixReceiver.cpp -o $RECV -ldl || exit 2
fi

# ------------------------------------------------------------------- displays
XVFB_PIDS=()
INSTANCES=()
cleanup() {
  for p in $INSTANCES $XVFB_PIDS; do kill $p 2>/dev/null; done
}
trap cleanup EXIT INT TERM
if [[ -z "$DISPLAYS" ]]; then
  for i in 1 2; do
    rm -f $WORK/xvfb$i.num
    Xvfb -displayfd 1 -screen 0 1920x1080x24 +extension GLX +render -noreset \
      > $WORK/xvfb$i.num 2>/dev/null &
    XVFB_PIDS+=$!
    for t in {1..50}; do [[ -s $WORK/xvfb$i.num ]] && break; sleep 0.1; done
    DISPLAYS="$DISPLAYS :$(head -1 $WORK/xvfb$i.num)"
  done
fi
DISP_S=${${=DISPLAYS}[1]} DISP_R=${${=DISPLAYS}[2]}
fi

# ------------------------------------------------------------------ instances
# Separate profiles for the two roles, kept across runs so the library scan
# of a fresh profile only happens once, and separate per graphics settings.
gfxkey=$(print -r -- "$GFX" | tr -c 'A-Za-z0-9\n' '_')
profile() {  # role api
  local d=$WORK/profile-$1-$2-${gfxkey:-default}
  mkdir -p $d/config/ossia
  {
    print "[score_plugin_gfx]"
    print "GraphicsApi=$2"
    for kv in ${=GFX}; do print $kv; done
  } > $d/config/ossia/score.conf
  print $d
}

LAST_PID=0
launch() {  # display api role script log -> LAST_PID
  local prof=$(profile $3 $2)
  local tmp=$(mktemp -d $WORK/tmp.XXXXXX)
  # SCORE_DISABLE_FAILSAFE: the instances are stopped with SIGTERM, which
  # leaves score's crash marker behind, and the next instance on that profile
  # would then start in failsafe mode with its GPU paths degraded.
  env DISPLAY=$1 XDG_CONFIG_HOME=$prof/config TMPDIR=$tmp QT_FORCE_STDERR_LOGGING=1 \
      SCORE_DISABLE_FAILSAFE=1 \
      LD_LIBRARY_PATH=$APP/plugins${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      $APP/ossia-score --no-restore --script "$4" > $5 2>&1 &
  LAST_PID=$!
  INSTANCES+=$LAST_PID
}

stop() {
  kill $1 2>/dev/null
  wait $1 2>/dev/null
  INSTANCES=(${INSTANCES:#$1})
}

# Wait until a score instance has published an NDI source, up to $2 seconds.
wait_source() {  # log seconds
  for t in {1..$2}; do
    grep -aq "NDIMATRIX: playing" $1 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

sender_js() {  # fmt
  cat <<EOF
Score.createDevice("NDIOut", "07651c13-83de-48b8-a450-abe2891051e8",
  {"Path": "$PREFIX-$1", "Width": $W, "Height": $H, "Rate": 60, "Format": "$1"});
var p = Score.createProcess(Score.rootInterval(), "74ca45ff-92c9-44a0-8f1a-754dea05ee1b",
  "$SELF_DIR/matrix/pattern.fs");
Score.setAddress(Score.outlet(p, 0), "NDIOut:/");
Score.play();
console.log("NDIMATRIX: playing");
EOF
}

receive_format_name() {
  case $1 in
    8bit) print "8-bit (SDK de-interlaces)";;
    best) print "Best available (16-bit, fields)";;
  esac
}

receiver_js() {  # fmt mode
  cat <<EOF
Score.createDevice("NDIIn", "ae78b7c6-6400-483e-b45b-fd6ff87ec700",
  {"Path": "$HOSTU ($PREFIX-$1)", "ReceiveFormat": "$(receive_format_name $2)"});
Score.createDevice("Relay", "07651c13-83de-48b8-a450-abe2891051e8",
  {"Path": "${PREFIX}relay-$1-$2", "Width": $W, "Height": $H, "Rate": 60, "Format": "RGBA"});
var p = Score.createProcess(Score.rootInterval(), "74ca45ff-92c9-44a0-8f1a-754dea05ee1b",
  "$SELF_DIR/matrix/passthrough.fs");
Score.setAddress(Score.inlet(p, 0), "NDIIn:/");
Score.setAddress(Score.outlet(p, 0), "Relay:/");
Score.play();
console.log("NDIMATRIX: playing");
EOF
}

# ------------------------------------------------------------------- verdicts
# Per family, on the 8-bit RGB the relay carries. The baseline of a correct
# cell is about 3 mean / 10-15 worst (8-bit rounding twice, 4:2:x chroma at
# the gradient); a wrong matrix costs 11+ mean, a wrong plane offset or a
# flipped picture far more.
#
# The RGB formats get more room: a source the SDK delivers as RGB goes through
# the SDK's lossy RGB -> YUV -> RGB round trip twice, once on the way into the
# receiver and once more on the relay, and each one shifts red by a few units
# (measured: 3/255 mean for one round trip, 5.9 for two, on every build).
thresholds() {  # fmt -> "mean worst"
  case $1 in
    RGBA|RGBX|BGRA|BGRX) print "7 20";;
    P216) print "5 20";;
    *) print "5 24";;
  esac
}
# Alpha: score renders every output over opaque black, so no output, NDI or
# other, ever has an alpha below 1; the pattern's alpha strip arrives as
# colour times alpha and is checked as such ("strip"). The relay's own alpha
# is printed in results.txt and is 255 for the same reason.

verdict() {  # fmt result-line -> cell text, starting with OK when it passes
  local fmt=$1 r=$2
  case "$r" in
    *nosource*|*noframe*|"") print "no picture"; return;;
    *unsupported*) print "no picture (${r#RESULT })"; return;;
  esac
  local mean=${${r#*mean=}%% *} worst=${${${r#*worst=}%%@*}%% *}
  local marker=${${r#*marker=}%% *}
  local strip=${${r#*strip=}%% *}
  local t=($(thresholds $fmt))
  local why=()
  (( mean < t[1] )) || why+="mean $mean"
  (( worst < t[2] )) || why+="worst $worst"
  (( marker == 1 )) || why+="no marker at top left"
  (( strip < t[1] )) || why+="alpha strip $strip"
  [[ "$r" == *failsafe* ]] && why+="an instance ran in failsafe mode"
  if (( ${#why} == 0 )); then
    print "OK ($mean / $worst)"
  else
    print "WRONG: ${(j:, :)why} ($mean / $worst)"
  fi
}

typeset -A CELLS
FAILS=0
if [[ -n "$RESCORE" ]]; then
  for F in ${=FORMATS}; do
    for M in ${=MODES}; do
      res=$(grep -a "^$SAPI->$RAPI $F $M: " $RESCORE | tail -1)
      CELLS[$F,$M]=$(verdict $F "${res#*: }")
      [[ ${CELLS[$F,$M]} == OK* ]] || FAILS=$((FAILS + 1))
    done
  done
fi

# ---------------------------------------------------------------------- run
for F in ${=FORMATS}; do
  [[ -n "$RESCORE" ]] && break
  slog=$WORK/sender-$SAPI-$F.log
  launch $DISP_S $SAPI sender "$(sender_js $F)" $slog
  sp=$LAST_PID
  if ! wait_source $slog 90; then
    for M in ${=MODES}; do CELLS[$F,$M]="no picture (sender did not start)"; done
    FAILS=$((FAILS + 1))
    stop $sp
    continue
  fi
  sleep 4
  for M in ${=MODES}; do
    rlog=$WORK/receiver-$RAPI-$F-$M.log
    launch $DISP_R $RAPI receiver "$(receiver_js $F $M)" $rlog
    rp=$LAST_PID
    wait_source $rlog 90
    sleep 6
    res=$(MATRIX_SAVE=$WORK/relay-$SAPI-$RAPI-$F-$M.ppm \
          $RECV $NDILIB "${PREFIX}relay-$F-$M)" 3 45 2>/dev/null | grep '^RESULT')
    grep -aq "starting in failsafe mode" $slog $rlog && res="$res failsafe"
    stop $rp
    print -r -- "$SAPI->$RAPI $F $M: $res" >> $WORK/results.txt
    CELLS[$F,$M]=$(verdict $F "$res")
    [[ ${CELLS[$F,$M]} == OK* ]] || FAILS=$((FAILS + 1))
    print -r -- "  $F x $M: ${CELLS[$F,$M]}"
  done
  stop $sp
done

# --------------------------------------------------------------------- table
{
  print
  print "### ${LABEL:-NDI two-instance matrix} -- sender $SAPI, receiver $RAPI, ${W}x${H}${GFX:+, $GFX}"
  print
  print -rn -- "| output format |"
  for M in ${=MODES}; do print -rn -- " receive: $(receive_format_name $M) |"; done
  print
  print -rn -- "|---|"
  for M in ${=MODES}; do print -rn -- "---|"; done
  print
  for F in ${=FORMATS}; do
    print -rn -- "| $F |"
    for M in ${=MODES}; do print -rn -- " ${CELLS[$F,$M]-} |"; done
    print
  done
  print
  print "Cells: mean / worst max-channel error, in 8-bit units, against the source picture."
} | tee -a ${OUT:-/dev/null}

(( FAILS == 0 ))
