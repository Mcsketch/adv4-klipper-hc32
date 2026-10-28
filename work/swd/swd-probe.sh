#!/usr/bin/env bash
# Non-destructive attach test, N32G455 (NA board). Every access is a read; each is wrapped in
# catch so a faulting read still lets the MCU resume.
# Usage: ./swd-probe.sh            (plain attach)
#        ./swd-probe.sh cur        (connect-under-reset; only if an NRST pad is wired to RST)
cd "$(dirname "$0")"
extra=(); [ "$1" = cur ] && extra=(-c "reset_config srst_only srst_nogate connect_assert_srst")
exec openocd -f ad4-swd.cfg "${extra[@]}" -c init -c halt \
  -c 'echo "=== CPUID @0xE000ED00  (expect 0x410fc24x = Cortex-M4)"'            -c 'catch {mdw 0xE000ED00}' \
  -c 'echo "=== DBGMCU_IDCODE @0xE0042000  (Nations device id, record it)"'     -c 'catch {mdw 0xE0042000}' \
  -c 'echo "=== vendor IAP bootloader vectors @0x08000000  (SP 0x2000xxxx, odd PC 0x0800xxxx)"' -c 'catch {mdw 0x08000000 4}' \
  -c 'echo "=== app vectors @0x08010000  (NA 2.3.0 hex: 0x20003b78 0x080101ad ...)"'          -c 'catch {mdw 0x08010000 4}' \
  -c 'echo "=== flash alias @0x00000000  (should equal 0x08000000 when booted from flash)"'   -c 'catch {mdw 0x00000000 4}' \
  -c 'echo "=== option bytes @0x1FFFF800  (low byte 0xa5 = no read protection; anything else = RDP on: STOP, do not unlock)"' -c 'catch {mdw 0x1FFFF800 4}' \
  -c resume -c shutdown
