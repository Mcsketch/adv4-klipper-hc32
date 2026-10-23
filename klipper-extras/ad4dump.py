# Flash read-out for the Adventurer 4's MCU.
#
# The vendor IAP bootloader that lives in HC32F460 flash 0x000000..0x00BFFF has
# no read command: the stock flasher `IAPCommand` contains no read/verify
# function at all, only write. So the only way to image that region is to read
# it from code already running on the MCU. Code flash is memory-mapped from
# 0x0, so klipper's built-in `debug_read` is a plain 32-bit load -- no firmware
# change needed, at four bytes per round trip.
#
#   ADFLASH_PEEK ADDR=0x0 [COUNT=64]
#   ADFLASH_DUMP START=0 END=0xC000 FILE=adv4-bootloader.bin
import os
import struct

# HC32F460: 512 KiB of code flash. Klipper's own .config says 0x40000, which is
# wrong for this part -- the top of the range here is the real one.
FLASH_TOP = 0x80000
WORD = 4

# klipper's dispatcher splits a command line with '([A-Z_]+|[A-Z*])' and takes
# the name as the first letter-run plus the next chunk, so any digit inside a
# name truncates it ("AD4_FLASH_PEEK" dispatches as "AD4"). Hence the digit-free
# names above, even though register_command() would accept the other spelling.
def _num(s):
    # Accepts 0xC000 and plain decimal, which is what makes the addresses
    # readable in a console.
    return int(s, 0)


def _hexdump(data, base=0):
    out = []
    for off in range(0, len(data), 16):
        line = data[off:off + 16]
        hexs = " ".join("%02x" % b for b in line)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in line)
        out.append("%06x  %-47s  %s" % (base + off, hexs, asc))
    return "\n".join(out)


class AD4Flash:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        self.gcode = self.printer.lookup_object('gcode')
        self.mcu = self.printer.lookup_object('mcu')
        self.read_cmd = None
        self.mcu.register_config_callback(self._build_config)
        self.gcode.register_command(
            "ADFLASH_PEEK", self.cmd_ADFLASH_PEEK,
            desc="Read MCU flash words and print a hexdump")
        self.gcode.register_command(
            "ADFLASH_DUMP", self.cmd_ADFLASH_DUMP,
            desc="Dump a range of MCU flash to a file")

    def _build_config(self):
        # debug_read is HF_IN_SHUTDOWN, so this still works on an MCU that has
        # already shut down -- which is exactly when a dump matters most.
        self.read_cmd = self.mcu.lookup_query_command(
            "debug_read order=%c addr=%u", "debug_result val=%u",
            cq=self.mcu.alloc_command_queue(), is_async=True)

    def read_block(self, addr, count):
        # readl() is a little-endian load, so the wire integer unpacks straight
        # back into memory order.
        out = bytearray()
        for off in range(0, count, WORD):
            val = self.read_cmd.send([2, addr + off])['val']
            out += struct.pack("<I", val)
        return bytes(out[:count])

    def _checked_range(self, gcmd, start, end):
        if start < 0 or start % WORD:
            raise gcmd.error("START must be a 4-byte aligned offset")
        if end <= start or end > FLASH_TOP:
            raise gcmd.error("END must be in (START, %d]" % FLASH_TOP)
        return start, end

    cmd_ADFLASH_PEEK_help = "Read MCU flash words and print a hexdump"

    def cmd_ADFLASH_PEEK(self, gcmd):
        addr = gcmd.get("ADDR", 0, parser=_num,
                        minval=0, maxval=FLASH_TOP - WORD)
        count = gcmd.get("COUNT", 64, parser=_num, minval=WORD, maxval=1024)
        if addr % WORD:
            raise gcmd.error("ADDR must be 4-byte aligned")
        data = self.read_block(addr, count)
        gcmd.respond_info("MCU flash %06x..%06x:\n%s"
                          % (addr, addr + count - 1, _hexdump(data, addr)))

    cmd_ADFLASH_DUMP_help = "Dump a range of MCU flash to a file"

    def cmd_ADFLASH_DUMP(self, gcmd):
        start, end = self._checked_range(
            gcmd, gcmd.get("START", 0, parser=_num),
            gcmd.get("END", FLASH_TOP, parser=_num))
        filename = os.path.expanduser(
            gcmd.get("FILE", "adv4-flash-%06x-%06x.bin" % (start, end)))
        total = end - start
        step = gcmd.get("CHUNK", 256, parser=_num, minval=WORD, maxval=4096)
        step -= step % WORD
        gcmd.respond_info("Reading %d bytes of MCU flash (%d round trips)"
                          % (total, total // WORD))
        data = bytearray()
        startclock = self.reactor.monotonic()
        while len(data) < total:
            n = min(step, total - len(data))
            data += self.read_block(start + len(data), n)
            if len(data) % (step * 8) == 0 or len(data) == total:
                elapsed = self.reactor.monotonic() - startclock
                gcmd.respond_info("  %d/%d bytes (%.0f s, %.1f B/s)"
                                  % (len(data), total, elapsed,
                                     len(data) / max(elapsed, .001)))
        with open(filename, "wb") as f:
            f.write(bytes(data))
        gcmd.respond_info("Wrote %s (%d bytes, %.0f s)"
                          % (filename, len(data),
                             self.reactor.monotonic() - startclock))


def load_config(config):
    return AD4Flash(config)
