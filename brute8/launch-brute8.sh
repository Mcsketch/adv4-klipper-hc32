#!/bin/bash
# =============================================================================
#  AD4 root-password DES brute — one-shot launch (GPU + all CPU cores)
# =============================================================================
#  Target : H9.zxVgLFpRG2   (traditional DES / Unix crypt(3), 8-byte max)
#  Engine : hashcat 7.x (brew, mode 1500) on the M1 Ultra GPU
#           + 20 CPU cores (brute8_cpu.py via /usr/bin/python3)
#
#  Coverage — 100% of the remaining random [a-z0-9] space (measured split):
#     GPU 8-char LETTER-start  26*36^7 = 2,037,468,266,496  [~12.3M/s]
#     CPU 8-char DIGIT-start   10*36^7 =  783,641,640,960   [20 workers]
#     GPU 7-char ALL           1*36^7  =   78,364,164,096   (~1.8h warm-up)
#     (6-char + word+digit spaces were already exhausted: no match.)
#
#  PAUSE (SAFE — state stays in RAM, no progress lost):
#     ./launch-brute8.sh pause    # SIGSTOP GPU+CPU (freeze at 0% -> use your Mac)
#     ./launch-brute8.sh resume   # SIGCONT (exactly where it stopped)
#     ./launch-brute8.sh status   # live progress
#     ./launch-brute8.sh stop     # hard stop
#
#  LAUNCH IT YOURSELF with Hermes + LM Studio closed so the machine is idle:
#     cd ~/adv4-klip/brute8 && ./launch-brute8.sh
#
#  A found password is written to ~/adv4-klip/brute8/FOUND.txt and the run
#  self-stops.
# =============================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HASH="H9.zxVgLFpRG2"
G_LCHAR="abcdefghijklmnopqrstuvwxyz"
G_ALL="abcdefghijklmnopqrstuvwxyz0123456789"
HASHCAT="$(command -v hashcat || echo /opt/homebrew/bin/hashcat)"
PY=/usr/bin/python3
NCPU=20
PIDS_FILE="$HERE/pids"
FOUND="$HERE/FOUND.txt"
GPU7_LOG="$HERE/gpu7.log"
GPU8_LOG="$HERE/gpu8.log"
MON_LOG="$HERE/monitor.log"

add_pid(){ echo "$1" >> "$PIDS_FILE"; }

start_gpu(){
  # One GPU pipeline: 7-char ALL first (~1.8h, full rate), then 8-char
  # LETTER-start (~2.0d, full rate). Sequential so the GPU never splits.
  (
    "$HASHCAT" -m 1500 -a 3 -w 4 -S --session ad4gpu7 \
       --custom-charset1="$G_ALL" -o "$FOUND" "$HASH" \
       '?1?1?1?1?1?1?1' < /dev/null >> "$GPU7_LOG" 2>&1
    if [ ! -s "$FOUND" ]; then
      "$HASHCAT" -m 1500 -a 3 -w 4 -S --session ad4gpu8 \
         --custom-charset1="$G_LCHAR" --custom-charset2="$G_ALL" \
         -o "$FOUND" "$HASH" '?1?2?2?2?2?2?2?2' < /dev/null >> "$GPU8_LOG" 2>&1
    fi
  ) </dev/null >/dev/null 2>&1 &
  add_pid $!; disown
}

start_cpu(){
  local total=$((10 * 36 ** 7))     # digit-start 8-char space
  local per=$(( total / NCPU ))
  local s c i
  for ((i=0; i<NCPU; i++)); do
    s=$(( i * per ))
    c=$(( (i==NCPU-1) ? total - s : per ))
    "$PY" "$HERE/brute8_cpu.py" "$s" "$c" "$FOUND" "$HERE/cpu_w${i}.log" \
       < /dev/null >> "$HERE/cpu_w${i}.log" 2>&1 &
    add_pid $!; disown
  done
}

monitor(){
  while true; do
    if [ -f "$FOUND" ] && [ -s "$FOUND" ]; then
      echo "[$(date +%T)] FOUND -> $(cat "$FOUND") | stopping all" >> "$MON_LOG"
      pkill -f "hashcat.*ad4gpu" 2>/dev/null
      pkill -f "brute8_cpu.py"   2>/dev/null
      break
    fi
    sleep 5
  done
}

cmd_start(){
  rm -f "$FOUND" "$PIDS_FILE"
  echo "==> AD4 DES brute launch  (GPU + $NCPU CPU cores)"
  echo "    target : $HASH"
  echo "    hashcat: $HASHCAT"
  echo "    GPU 8-letter : $((26*36**7)) cands  [~12.3M/s]"
  echo "    CPU 8-digit  : $((10*36**7)) cands  [$NCPU workers]"
  echo "    GPU 7-char   : $((36**7)) cands (~1.8h warm-up)"
  echo
  start_gpu
  start_cpu
  monitor >> "$MON_LOG" 2>&1 < /dev/null & add_pid $!; disown
  echo "    all started  ->  $PIDS_FILE"
  echo "    FOUND file    :  $FOUND"
  echo "    logs          :  $GPU7_LOG $GPU8_LOG cpu_w*.log"
  echo
  echo "    PAUSE (safe):  $0 pause     RESUME: $0 resume     STATUS: $0 status     STOP: $0 stop"
}

cmd_pause(){
  [ -f "$PIDS_FILE" ] || { echo "no run (no pids file)"; exit 1; }
  echo "pausing (SIGSTOP) GPU + CPU..."
  while read -r p; do kill -STOP "$p" 2>/dev/null; done < "$PIDS_FILE"
  pkill -STOP -f "hashcat.*ad4gpu" 2>/dev/null
  pkill -STOP -f "brute8_cpu.py"   2>/dev/null
  echo "paused (frozen at 0%). machine is free.  resume: $0 resume"
}

cmd_resume(){
  [ -f "$PIDS_FILE" ] || { echo "no run"; exit 1; }
  echo "resuming (SIGCONT)..."
  while read -r p; do kill -CONT "$p" 2>/dev/null; done < "$PIDS_FILE"
  pkill -CONT -f "hashcat.*ad4gpu" 2>/dev/null
  pkill -CONT -f "brute8_cpu.py"   2>/dev/null
  echo "resumed from exactly where it stopped (no progress lost)."
}

cmd_stop(){
  echo "hard stop (SIGTERM)..."
  [ -f "$PIDS_FILE" ] && { while read -r p; do kill -TERM "$p" 2>/dev/null; done < "$PIDS_FILE"; }
  pkill -TERM -f "hashcat.*ad4gpu" 2>/dev/null
  pkill -TERM -f "brute8_cpu.py"   2>/dev/null
  sleep 1
  pkill -KILL -f "hashcat.*ad4gpu" 2>/dev/null
  pkill -KILL -f "brute8_cpu.py"   2>/dev/null
  echo "stopped."
}

cmd_status(){
  echo "=== AD4 DES brute status ==="
  [ -f "$FOUND" ] && [ -s "$FOUND" ] && echo "FOUND: $(cat "$FOUND")"
  if [ -f "$PIDS_FILE" ]; then
    local n=0 p; while read -r p; do kill -0 "$p" 2>/dev/null && n=$((n+1)); done < "$PIDS_FILE"
    echo "processes alive: $n"
  fi
  echo "CPU:  $(top -l 1 -n 0 2>/dev/null | grep 'CPU usage' | tr -d '\n')"
  echo "GPU 7-char:"; grep -iE "Progress|Status|Recovered" "$GPU7_LOG" 2>/dev/null | tail -1
  echo "GPU 8-char:"; grep -iE "Progress|Status|Recovered" "$GPU8_LOG" 2>/dev/null | tail -1
  echo "CPU worker[0]:"; tail -1 "$HERE/cpu_w0.log" 2>/dev/null
}

case "${1:-start}" in
  start)  cmd_start ;;
  pause)  cmd_pause ;;
  resume) cmd_resume ;;
  stop)   cmd_stop ;;
  status) cmd_status ;;
  *) echo "usage: $0 {start|pause|resume|stop|status}"; exit 1 ;;
esac
