# Shared by the install and rollback sticks (busybox ash on the AD4 SoC).
# Sourced by flashforge_init.sh with STICK (the stick mount) and LOG set.
#
# Context: /opt/auto_run.sh mounts the stick on /mnt and runs
# /mnt/flashforge_init.sh BEFORE it starts the vendor UI or the Klipper boot,
# so /dev/ttyS1 and the panel are free. Exit 0 parks the boot
# (`sleep 100000`), anything else lets it continue as normal.

D=/opt/ad4-klipper
P=$STICK/ad4-klipper
A=/opt/auto_run.sh
STOCK_LINE='/opt/PROGRAM/ffstartup-arm -f /opt/PROGRAM/ffstartup.cfg &'
# Without klipper-mode the stock UI starts, with SSH still up for recovery.
KLIPPER_LINE="if [ -f $D/klipper-mode ]; then $D/klipper-boot.sh & else /bin/dropbear -R -p 22; $STOCK_LINE fi"
# The only IAPCommand this project has flashed with (FlashForge 2.3.0 kit).
IAP_MD5=919864f702cf3c07665234ab8de7896e

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >>"$LOG"; sync; }
screen() { AD4_MCU="$BOARD" "$P/ad4-screen" "$@" >>"$LOG" 2>&1; }
buzz() { "$P/ad4-buzz" "$1" >/dev/null 2>&1; }
md5() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

# Stop here and leave the panel showing why. Exit 0 parks the boot, so the
# vendor UI cannot paint over the message; the user power-cycles.
fail() {
    log "FAILED: $1"
    screen fail "$1" "$2"
    buzz fail
    cp /tmp/iap/iap.log "$STICK/iap.log" 2>/dev/null
    sync
    exit 0
}

# Nothing changed: let the printer boot as it would without the stick.
cancel() {
    log "cancelled: $1"
    screen fail "Cancelled" "$1. Nothing was changed. Remove the stick and power-cycle."
    sync
    sleep 8
    exit 1
}

# /root/N32 or /root/H32 is touched by IAPCommand after a successful write.
board() {
    if [ -f /root/H32 ]; then echo HD
    elif [ -f /root/N32 ]; then echo NA
    else echo unknown; fi
}

# FlashForge's own MCU kit, shipped on every printer: IAPCommand plus the
# stock NA and HD hex files, with an md5sum.list. Sets KIT NA_HEX HD_HEX.
find_kit() {
    KIT=
    for k in /opt/PROGRAM/control/*/; do
        k=${k%/}
        [ -f "$k/IAPCommand" ] && [ -f "$k/md5sum.list" ] || continue
        ok=1
        while read -r sum name; do
            [ -n "$name" ] || continue
            if [ "$(md5 "$k/${name#./}")" != "$sum" ]; then
                log "kit $k: md5 mismatch on $name"; ok=0
            fi
        done <"$k/md5sum.list"
        [ $ok = 1 ] || continue
        NA_HEX=$(ls "$k"/*NA-IAP*.hex 2>/dev/null | head -1)
        HD_HEX=$(ls "$k"/*HD-IAP*.hex 2>/dev/null | head -1)
        [ -n "$NA_HEX" ] && [ -n "$HD_HEX" ] || continue
        KIT=$k
    done
    [ -n "$KIT" ] && log "stock kit: $KIT (IAPCommand md5 $(md5 "$KIT/IAPCommand"))"
    [ -n "$KIT" ]
}

busy() {
    ps | grep -v grep | grep -E 'ffstartup-arm|adventurer4-arm|IAPCommand|ad4-bridge|ad4-daemon'
}

# 1 KiB frames: IAPCommand prints one line per frame written.
frames() {
    awk 'BEGIN { x = "0123456789ABCDEF" }
         substr($0, 8, 2) == "00" {
             h = toupper(substr($0, 2, 2))
             n += (index(x, substr(h, 1, 1)) - 1) * 16 + index(x, substr(h, 2, 1)) - 1
         }
         END { print int((n + 1023) / 1024) }' "$1"
}

# iap NA_FILE HD_FILE TITLE: IAPCommand resets the MCU into its bootloader
# (GPIO34/35), erases the app and writes whichever file matches the chip.
# The bootloader itself is never touched, so a failed write is fixed by
# running this again. Returns 0 = NA board written, 3 = HD board written,
# 1 = failed.
iap() {
    rm -rf /tmp/iap
    mkdir -p /tmp/iap || return 1
    cp "$KIT/IAPCommand" /tmp/iap/IAPCommand && chmod +x /tmp/iap/IAPCommand
    cp "$1" "/tmp/iap/$(basename "$1")" && cp "$2" "/tmp/iap/$(basename "$2")" || return 1
    total=$(frames "$1")
    [ "$total" -gt 0 ] 2>/dev/null || total=1
    log "IAPCommand $(basename "$1") $(basename "$2") ($total frames)"
    (cd /tmp/iap && ./IAPCommand "$(basename "$1")" "$(basename "$2")" >iap.log 2>&1
     echo "EXIT=$?" >>iap.log) &
    pid=$!
    t=0
    while kill -0 $pid 2>/dev/null; do
        sleep 2; t=$((t + 2))
        n=$(grep -c 'frame data transmission is completed' /tmp/iap/iap.log)
        pct=$((n * 100 / total))
        [ $pct -gt 99 ] && pct=99
        screen status "$3" "frame $n of $total. Do not power off." $pct
        if [ $t -ge 900 ]; then log "IAPCommand still running after 900 s"; break; fi
    done
    # IAPCommand leaves the port in even parity; Klipper needs 8N1.
    stty -F /dev/ttyS1 -parenb 2>/dev/null
    cp /tmp/iap/iap.log "$STICK/iap.log" 2>/dev/null
    log "iap.log: $(grep -v 'frame data' /tmp/iap/iap.log | tr '\n' '|')"
    grep -q '^EXIT=0' /tmp/iap/iap.log || return 1
    grep -q 'STM32iap finished NA' /tmp/iap/iap.log && return 0
    grep -q 'STM32iap finished HD' /tmp/iap/iap.log && return 3
    return 1
}

# set_launch klipper|stock: rewrite the one line of /opt/auto_run.sh that
# starts the printer's software. Any earlier Klipper line (this project or
# the older ad4-daemon installer) and a standalone dropbear line are
# replaced too. Written to a new file and renamed, so the shell that is
# running the current auto_run.sh keeps reading the old one.
set_launch() {
    if [ "$1" = klipper ]; then new=$KLIPPER_LINE; else new=$STOCK_LINE; fi
    [ -f "$A.pre-ad4-klipper" ] || cp -p "$A" "$A.pre-ad4-klipper" || return 1
    awk -v stock="$STOCK_LINE" -v new="$new" '
        { l = $0; sub(/^[ \t]+/, "", l); sub(/[ \t]+$/, "", l) }
        l == stock || index(l, "/opt/ad4-klipper/") {
            if (!done) print new
            done = 1; next
        }
        l == "/bin/dropbear -R -p 22 &" { next }
        { print }
        END { exit done ? 0 : 1 }' "$A" >"$A.new" || { rm -f "$A.new"; return 1; }
    [ "$(grep -cxF "$new" "$A.new")" = 1 ] || { rm -f "$A.new"; return 1; }
    if [ "$1" = stock ] && grep -q '/opt/ad4-klipper/' "$A.new"; then rm -f "$A.new"; return 1; fi
    sh -n "$A.new" || { rm -f "$A.new"; return 1; }
    chmod 755 "$A.new" && mv -f "$A.new" "$A" && sync
}

# Done: the stick must not run again on the next boot.
retire() {
    mv -f "$STICK/flashforge_init.sh" "$STICK/flashforge_init.sh.done"
    sync
}
