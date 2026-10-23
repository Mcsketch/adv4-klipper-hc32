#!/bin/sh
# ad4-setrootpw.sh — change the ADV4 SoC root password. Runs ON the SoC
# (busybox sh, no python). Verifies by diffing the /etc/shadow hash.
#
#   already on the SoC:
#     sh ad4-setrootpw.sh 'newpass'
#   from the Mac (two steps: upload, then run with the new password):
#     sshpass -p OLD ssh -o StrictHostKeyChecking=no root@192.168.10.10 \
#         'cat > /tmp/ad4-setrootpw.sh' < ad4-setrootpw.sh
#     sshpass -p OLD ssh -o StrictHostKeyChecking=no root@192.168.10.10 \
#         "sh /tmp/ad4-setrootpw.sh 'newpass'"
#
# The password is never read from stdin: over `ssh 'sh -s' < script` stdin IS
# the script, so an interactive read would eat its own source.
# If dropbear is not already persisted in /opt/auto_run.sh this adds it —
# otherwise a password change made over SSH is a lockout risk on reboot.
NEW="${1:-${AD4_NEWPW:-}}"
if [ -z "$NEW" ] && [ -t 0 ]; then
    printf 'new root password: '
    read NEW
fi
[ -n "$NEW" ] || { echo "usage: sh ad4-setrootpw.sh 'newpass'   (or AD4_NEWPW=...)"; exit 1; }
[ "${#NEW}" -ge 4 ] || { echo "password too short (<4), aborting"; exit 1; }

OLD=$(awk -F: '$1=="root"{print $2}' /etc/shadow 2>/dev/null)
[ -n "$OLD" ] || echo "WARNING: root shadow entry is empty or locked"

printf '%s\n%s\n' "$NEW" "$NEW" | passwd >/dev/null 2>&1
NOW=$(awk -F: '$1=="root"{print $2}' /etc/shadow 2>/dev/null)

if [ "$NOW" = "$OLD" ] || [ -z "$NOW" ]; then
    echo "FAILED: /etc/shadow root entry did not change"
    echo "        (busybox passwd refused; boot the USB stick instead —"
    echo "         flashforge_init.sh forces the shadow hash for the default pw)"
    exit 1
fi
echo "root password changed (/etc/shadow root entry rewritten)"

# Keep SSH reachable after a reboot, or a new password is a lockout risk.
if [ -f /opt/auto_run.sh ] && ! grep -qF "dropbear" /opt/auto_run.sh 2>/dev/null; then
    cp -f /opt/auto_run.sh /opt/auto_run.sh.ad4-orig 2>/dev/null
    echo "/bin/dropbear -R -p 22 &" >> /opt/auto_run.sh
    echo "dropbear start line added to /opt/auto_run.sh (was missing)"
fi
if ! grep -qiE ':0016[[:space:]]' /proc/net/tcp 2>/dev/null; then
    echo "NOTE: nothing is listening on :22 right now — start it with"
    echo "      /bin/dropbear -R -p 22"
fi
echo "test it:  ssh root@$(grep -m1 -o '192\.168\.10\.[0-9]*' /opt/ad4-klipper/ad4-net.conf 2>/dev/null || echo 192.168.10.10)"
