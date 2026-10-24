/*
 * stm32isp-ad4 — drive the Flashforge Adventurer 4 MCU's vendor IAP bootloader
 * from the SoC, including the board GPIO strapping a generic tool cannot know.
 *
 * Everything protocol-shaped in here is transcribed from the factory
 * "IAPCommand" binary (new-usb-flash/IAPCommand), which is UNSTRIPPED and
 * carries debug_info — so the flow below is read off its disassembly, not
 * inferred. Its main():
 *
 *   serialOpen("/dev/ttyS1", 115200)
 *   stm32Unexport()
 *   stm32UpdateFLag(1)   == gpio_out(35, 1)
 *   sleep(1)
 *   stm32Init()          == gpio_out(34,0); usleep(15680);
 *                          gpio_set_value(34,1); usleep(15680); stm32Sync()
 *   readHexFile()        -> mmap list of 1024-byte nodes, each memset 0xFF
 *   stm32Write()         -> per node: fillPacket, write, WAIT for 0x06,
 *                           retransmit that same frame until it arrives
 *   write 0x04 "write complete", again waiting for 0x06
 *   stm32Close(); stm32UpdateFLag(0); system("touch /root/H32"|"N32")
 *
 * There is NO read, verify or erase function anywhere in that binary. Its full
 * symbol list: main readHexFile fillPacket crc16 mmapInit mmapPut mmapDump
 * mmapDelete serialOpen serialFlush serialClose serialWrite serialRead
 * stm32Init stm32Sync stm32Connected stm32Write stm32Reboot stm32Close
 * stm32Unexport stm32UpdateFLag gpio_export gpio_unexport gpio_set_direction
 * gpio_set_value gpio_out.  So this bootloader has no read path at all, and the
 * STM32 ROM ops (0x02 get / 0x11 read / 0x44 erase / 0x46 program / 0x08 jump)
 * are not spoken here — an earlier version of this tool drove them and produced
 * a 256 KiB file of zeroes. Flash is dumped through the *running Klipper
 * firmware* instead: ADFLASH_DUMP in klipper-extras/ad4dump.py, which uses
 * klipper's built-in debug_read at ~1075 B/s (measured 2026-10-23: the 48 KiB
 * bootloader window in 46 s, all 512 KiB in ~7.5 min). That path is proven end
 * to end — firmware/dumps/adv4-bootloader-0x0-0xC000.bin came from it, and the
 * same read reproduced out/klipper.bin byte for byte at 0x0000C000, so the
 * write path in this file and the read path there are both correct.
 *
 * The bootloader is FLASH-RESIDENT, not mask ROM: the app window starts at
 * 0x0000C000 and whatever answers 0x43 lives below it. It is therefore
 * erasable, i.e. brickable. The `off < 0x00008000` guard below is the only
 * thing between a bad hex file and a dead board. It is NOT read-protected —
 * the vector table at 0x0 reads back fine — and it is now imaged, so a wipe is
 * recoverable by re-flashing that file here.
 *
 * Build (cross from the Mac — the SoC has no compiler):
 *   zig build-exe -target arm-linux-musleabi -mcpu=cortex_a7 -lc -static \
 *       -OReleaseSmall stm32isp-ad4.c -femit-bin=stm32isp-ad4_arm
 * Usage:
 *   stm32isp-ad4 [-p /dev/ttyS1] [-v] <hex>        flash one image
 *   stm32isp-ad4 [-p /dev/ttyS1] -s                revision probe, writes nothing
 */

/* Feature-test macros: POSIX symbols (usleep, cfmakeraw, strcasecmp) under
 * both musl and glibc. Must precede ALL includes. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <time.h>

/* ---- SoC GPIO strapping (the board-specific part) ----------------------
 * Both numbers come straight out of IAPCommand: gpio_out(34, ...) is the NRST
 * pulse inside stm32Init, and stm32UpdateFLag(n) is literally gpio_out(35,n) —
 * high for the whole flash, low again at the end. What GPIO 35 does on the MCU
 * side is still NOT proven (the HC32F460 has no pin called BOOT0); all this
 * tool needs is the proven behaviour: high before the reset pulse, or the
 * bootloader stays silent. */
#define NRST_GPIO   34
#define BOOT0_GPIO  35      /* label only — see above */
#define NRST_US     15680   /* IAPCommand's usleep(15680) */
#define GPIO_ROOT   "/sys/class/gpio"

/* ---- vendor IAP protocol ----------------------------------------------
 * Sync:   host -> 0x43 ; mcu -> one byte, 0x07 = HD, 0x06 = NA. That byte is
 *         also the revision detector, which is why the factory tool ends with
 *         touch /root/H32 or /root/N32.
 * Data:   1030-byte frames, EVERY one ACKed by 0x06 from the MCU:
 *   [0]         0x01 = data chunk
 *   [1]         frame sequence, 0-based, running across the whole image
 *   [2:3]       payload length, little-endian
 *   [4:1027]    data, 0xFF-padded (mmapPut memsets each node 0xFF)
 *   [1028:1029] CRC-16/XMODEM, little-endian, over the FULL 1024-byte data
 *               region including the padding
 * Final:  one byte 0x04, also ACKed with 0x06.
 * The frame carries NO address: the bootloader writes each chunk at the next
 * location in its own app window, auto-erasing as it goes. That is why the
 * factory tool feeds one file to both the NA and HD slots and why there is no
 * separate erase command.
 * Frame layout and CRC verified byte-for-byte against a live strace of
 * IAPCommand (28 frames, all CRCs match); the ack/retransmit loop comes from
 * stm32Write itself, where serialRead() is followed by a `cmp 0x06` and a
 * "send error. again" branch back to the write. */
#define CMD_SYNC       0x43
#define SYNC_REV_HD    0x07
#define SYNC_REV_NA    0x06
#define IAP_FRAME_OK   0x06   /* same value as the NA sync byte; phase disambiguates */
#define IAP_CMD_DATA   0x01
#define IAP_CMD_FINAL  0x04
#define IAP_DATA_MAX   1024
#define IAP_FRAME_LEN  1030
#define FRAME_RETRIES  20

/* HC32F460 flash is 512 KiB: 0x000000..0x07FFFF. (klipper-hc32's
 * .config still says CONFIG_FLASH_SIZE=0x40000 — that is wrong too.) */
#define FLASH_TOP      0x00080000u
/* Anything at or below this lands on the flash-resident bootloader. */
#define BOOTLOADER_TOP 0x00008000u

/* ---- tiny sysfs GPIO helpers ------------------------------------------ */
static int g_exported[64];

static void die(const char *s){ perror(s); exit(1); }
static void wr(const char *p, const char *v){
    int fd = open(p, O_WRONLY); if(fd<0) die(p);
    write(fd, v, strlen(v)); close(fd);
}
static void gpio_export(int n){
    char dp[128];
    snprintf(dp, sizeof dp, GPIO_ROOT "/gpio%d/direction", n);
    if(access(dp, F_OK) != 0){
        char p[128], b[16];
        snprintf(p, sizeof p, GPIO_ROOT "/export");
        snprintf(b, sizeof b, "%d", n);
        int fd = open(p, O_WRONLY); if(fd<0) die("gpio export");
        if(write(fd, b, strlen(b)) < 0){ close(fd); die("gpio export write"); }
        close(fd);
        g_exported[n] = 1;
    }
    int fd = open(dp, O_WRONLY);
    if(fd >= 0){ write(fd, "out", 3); close(fd); }
    usleep(20000);
}
static void gpio_set(int n, int v){
    char p[128];
    snprintf(p, sizeof p, GPIO_ROOT "/gpio%d/value", n);
    wr(p, v ? "1" : "0");
}
static void gpio_unexport(int n){
    if(!g_exported[n]) return;
    char p[128], b[16];
    snprintf(p, sizeof p, GPIO_ROOT "/unexport");
    snprintf(b, sizeof b, "%d", n);
    int fd = open(p, O_WRONLY); if(fd<0) return;
    write(fd, b, strlen(b)); close(fd);
}

/* NRST: LOW 15.68 ms then HIGH 15.68 ms — exactly IAPCommand's stm32Init.
 * Only GPIO 34 is touched here; the vendor tool never pulses anything else. */
static void nrst_pulse(void){
    gpio_set(NRST_GPIO, 0); usleep(NRST_US);
    gpio_set(NRST_GPIO, 1); usleep(NRST_US);
}

/* ---- serial (115200 8N1 raw) ------------------------------------------ */
static int ser_open(const char *port){
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd<0){ fprintf(stderr,"open %s: %s\n",port,strerror(errno)); return -1; }
    struct termios t;
    if(tcgetattr(fd,&t)){ close(fd); return -1; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CSTOPB;
    t.c_cflag &= ~PARENB;
    t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    return fd;
}
/* Read up to n bytes within ms. Returns the byte count actually read. */
static int ser_rd(int fd, unsigned char *b, int n, int ms){
    int got=0;
    while(got<n && ms-->=0){
        int r = read(fd, b+got, n-got);
        if(r>0){ got+=r; continue; }
        if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) break;
        usleep(1000);
    }
    return got;
}
/* The port is O_NONBLOCK, so back-to-back frames can outrun the UART TX
 * buffer and write() returns EAGAIN. IAPCommand's port is blocking, so its
 * write() paces at UART speed naturally; reproduce that pacing explicitly. */
static int ser_wr(int fd, const unsigned char *b, int n){
    int o=0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while(o<n){
        int w = write(fd, b+o, n-o);
        if(w>0){ o+=w; continue; }
        if(w<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            poll(&pfd, 1, 50);
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long used = (now.tv_sec-t0.tv_sec)*1000 + (now.tv_nsec-t0.tv_nsec)/1000000;
            if(used > 10*1000){ errno=EAGAIN; return -1; }
            continue;
        }
        return -1;
    }
    return o;
}

/* ---- CRC-16/XMODEM, exactly IAPCommand's crc16() ----------------------
 * init 0, MSB-first, poly 0x1021, non-reflected. */
static uint16_t crc16_iap(const unsigned char *p, int n){
    uint16_t c=0;
    for(int i=0;i<n;i++){
        c ^= (uint16_t)p[i] << 8;
        for(int b=0;b<8;b++)
            c = (c & 0x8000) ? (uint16_t)((c<<1) ^ 0x1021) : (uint16_t)(c<<1);
    }
    return c;
}

/* One frame, with the vendor's wait-for-0x06-and-retransmit loop. For
 * IAP_CMD_FINAL, data/dlen are unused: that one is a bare 0x04 byte. */
static int iap_xmit(int fd, int cmd, unsigned char seq,
                    const unsigned char *data, int dlen, int verbose){
    unsigned char pkt[IAP_FRAME_LEN], ack=0;
    int plen = 1;
    if(cmd != IAP_CMD_FINAL){
        memset(pkt, 0xFF, sizeof pkt);
        pkt[0] = (unsigned char)cmd;
        pkt[1] = seq;
        pkt[2] = (unsigned char)(dlen & 0xff);
        pkt[3] = (unsigned char)((dlen >> 8) & 0xff);
        memcpy(pkt + 4, data, dlen);
        uint16_t crc = crc16_iap(pkt + 4, IAP_DATA_MAX);
        pkt[1028] = (unsigned char)(crc & 0xff);
        pkt[1029] = (unsigned char)((crc >> 8) & 0xff);
        plen = IAP_FRAME_LEN;
    } else {
        pkt[0] = IAP_CMD_FINAL;
    }
    for(int retry=0; retry<FRAME_RETRIES; retry++){
        if(ser_wr(fd, pkt, plen) != plen){
            fprintf(stderr,"write failed (seq=%d): %s\n", seq, strerror(errno));
            return 0;
        }
        /* serialRead()'s own select() timeout is 3 s; a timeout returns
         * non-zero and stm32Write re-sends the same frame. */
        if(ser_rd(fd, &ack, 1, 3000) == 1 && ack == IAP_FRAME_OK)
            return 1;
        if(verbose)
            fprintf(stderr,"  seq %d not acked (%s 0x%02x) — retransmit %d/%d\n",
                    seq, ack ? "reply" : "timeout", ack, retry+1, FRAME_RETRIES);
        tcflush(fd, TCIOFLUSH);
    }
    fprintf(stderr,"gave up on seq %d after %d retransmits\n", seq, FRAME_RETRIES);
    return 0;
}

/* Program the image as 0x01 frames, then close it with 0x04. */
static int iap_flash(int fd, const unsigned char *data, uint32_t len, int verbose){
    unsigned char seq=0;
    for(uint32_t o=0; o<len; o+=IAP_DATA_MAX){
        int n = (len-o)<IAP_DATA_MAX ? (int)(len-o) : IAP_DATA_MAX;
        if(!iap_xmit(fd, IAP_CMD_DATA, seq, data+o, n, verbose)){
            fprintf(stderr,"aborted at image offset 0x%06x\n", o);
            return 0;
        }
        seq++;
        if(verbose && (seq % 16)==0) fprintf(stderr,"  %u/%u bytes\n", o+n, len);
    }
    if(!iap_xmit(fd, IAP_CMD_FINAL, 0, NULL, 0, verbose)){
        fprintf(stderr,"final 0x04 (write complete) not acked\n");
        return 0;
    }
    return 1;
}

/* ---- minimal Intel-HEX parser ----------------------------------------- */
static int hx(char c){
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return 0;
}
static int hexn(const char *s, int n){
    int v=0; for(int i=0;i<n;i++) v=(v<<4)|hx(s[i]); return v;
}
typedef struct { uint32_t base; uint8_t *data; uint32_t len; } ihex_t;

static void ihex_load(const char *path, ihex_t *h, int *err){
    FILE *f=fopen(path,"rb");
    if(!f){ fprintf(stderr,"open %s: %s\n",path,strerror(errno)); *err=1; return; }
    uint32_t cap=1u<<20; uint8_t *buf=malloc(cap);
    uint32_t base=0, maxa=0, minabs=~0u; int have=0;
    char line[1024];
    while(fgets(line,sizeof line,f)){
        line[strcspn(line,"\r\n")]=0;
        if(line[0]!=':' || (int)strlen(line)<11) continue;
        int cnt=hexn(line+1,2);
        uint16_t off=hexn(line+3,4);
        int typ=hexn(line+7,2);
        if(typ==0x04 || typ==0x05){ base=((uint32_t)hexn(line+9,4))<<16; continue; }
        if(typ==0x01) break;
        /* Type 02 (extended SEGMENT) is rejected, not skipped. readHexFile in
         * IAPCommand frees and ignores those records, so an objcopy image that
         * straddles a 64K page gets its second page written at 0x00000000 —
         * straight onto the bootloader. tools/make-positioned-ihex.py emits one
         * type-04 per page instead, which is what the factory images use. */
        if(typ==0x02){
            fprintf(stderr,"%s: type-02 (extended segment) record — refusing. "
                            "Regenerate with tools/make-positioned-ihex.py\n", path);
            free(buf); *err=1; return;
        }
        if(typ!=0x00 || (int)strlen(line) < 5+cnt*2+5) continue;
        for(int i=0;i<cnt;i++){
            uint32_t abs=base+(uint32_t)off+i;
            if(!have || abs<minabs) minabs=abs;
            if(abs>=cap){ while(abs>=cap) cap<<=1; buf=realloc(buf,cap); }
            buf[abs-minabs]=(uint8_t)hexn(line+9+2*i,2);
            if(abs>maxa) maxa=abs;
            have=1;
        }
    }
    fclose(f);
    if(!have){ free(buf); *err=1; return; }
    h->base=minabs; h->data=buf; h->len=maxa-minabs+1; *err=0;
}

static void touch_path(const char *p){ (void)open(p, O_CREAT|O_WRONLY, 0644); }

/* Leave bootloader state and let the new image run. */
static void leave_bootloader(void){
    gpio_set(BOOT0_GPIO, 0);
    nrst_pulse();
    gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
}

/* ---- main -------------------------------------------------------------- */
int main(int argc, char **argv){
    const char *port = "/dev/ttyS1";
    const char *img = NULL;
    int verbose=0, probe_only=0;

    for(int i=1; i<argc; i++){
        if(!strcmp(argv[i],"-p")){
            if(++i>=argc) die("-p needs <port>");
            port=argv[i];
        } else if(!strcmp(argv[i],"-s")){
            probe_only=1;
        } else if(!strcmp(argv[i],"-v")){
            verbose=1;
        } else if(!strcmp(argv[i],"-d")){
            fprintf(stderr,"-d is gone: this bootloader has no read op at all "
                           "(IAPCommand contains no read or verify function).\n"
                           "Dump flash through the running Klipper firmware:\n"
                           "   ADFLASH_DUMP START=0 END=0x80000 FILE=mcu-flash.bin\n"
                           "   (klipper-extras/ad4dump.py, ~1075 B/s)\n");
            return 2;
        } else if(!strcmp(argv[i],"-r")){
            fprintf(stderr,"-r is gone: the revision comes from the sync reply "
                           "(0x07=HD, 0x06=NA) and the wire protocol carries no "
                           "address, so there is nothing to override.\n");
            return 2;
        } else if(argv[i][0]=='-' && argv[i][1]){
            fprintf(stderr,"unknown option %s\n", argv[i]); return 2;
        } else {
            img = argv[i];
        }
    }
    if(!img && !probe_only){
        fprintf(stderr,"usage: %s [-p /dev/ttyS1] [-v] <hex>\n", argv[0]);
        fprintf(stderr,"       %s [-p /dev/ttyS1] -s        (probe revision, "
                       "writes nothing)\n", argv[0]);
        return 2;
    }
    if(geteuid()!=0){ fprintf(stderr,"must run as root (GPIO + %s)\n",port); return 2; }

    /* 1) IAPCommand's exact order: open the port, then strap, then reset. */
    int fd = ser_open(port);
    if(fd<0) return 1;
    gpio_export(BOOT0_GPIO); gpio_export(NRST_GPIO);
    gpio_set(BOOT0_GPIO, 1);
    sleep(1);
    nrst_pulse();

    /* 2) sync. The MCU answers 0x43 with one byte, which is also the revision
     * detector. Re-pulse NRST periodically: the bootloader only listens right
     * after a reset taken while GPIO 35 is high. */
    int connected=0; unsigned char rev=0;
    /* A probe runs on every boot before the panel menu, so it has to give up
     * fast when the MCU will not answer; only a real flash may wait. */
    for(int tries=probe_only?8:60; tries && !connected; tries--){
        tcflush(fd, TCIOFLUSH);
        unsigned char c = CMD_SYNC;
        if(ser_wr(fd,&c,1) != 1) break;
        unsigned char b=0;
        if(ser_rd(fd,&b,1,300)==1 && (b==SYNC_REV_HD || b==SYNC_REV_NA)){
            rev=b; connected=1; break;
        }
        if(tries%5==0) nrst_pulse();
    }
    if(!connected){
        fprintf(stderr,"Can not connect to board, %s timed-out.\n",port);
        fprintf(stderr,"(0x43 is answered with 0x07=HD / 0x06=NA; silence means "
                       "the MCU is not in bootloader state)\n");
        leave_bootloader();
        return 1;
    }
    fprintf(stderr,"Connected to board. (sync reply 0x%02x = %s)\n", rev,
            rev==SYNC_REV_HD ? "HD" : "NA");
    if(probe_only){
        /* one greppable line for the installer, which shows this on the panel */
        fprintf(stderr,"REV=%s\n", rev==SYNC_REV_HD ? "HD" : "NA");
        leave_bootloader(); close(fd); return 0;
    }

    /* 3) load the image. */
    ihex_t h; int err=0; ihex_load(img,&h,&err);
    if(err){ fprintf(stderr,"failed to parse %s\n",img); leave_bootloader(); return 1; }

    /* The wire protocol is self-addressing, so the hex base is only a sanity
     * check on the file. Normalise the NA image's absolute 0x08xxxxxx form
     * down to a flash offset the same way IAPCommand's mmapPut does.
     * Subtract, do not mask: the stock NA image sits at 0x08010000, and
     * masking to 16 bits turns that into 0x000000, which the guard below
     * would read as "this image overwrites the bootloader" and refuse. */
    uint32_t off = (h.base >= 0x08000000u) ? (h.base - 0x08000000u) : h.base;
    fprintf(stderr,"loaded %s: %u bytes @ 0x%08x..0x%08x (flash offset "
                    "0x%06x..0x%06x)\n", img, h.len, h.base, h.base+h.len-1,
                    off, off+h.len-1);

    /* SAFETY: never write the flash-resident bootloader. It is the one mistake
     * that cannot be undone from the SoC. */
    if(off < BOOTLOADER_TOP){
        fprintf(stderr,"REFUSING: image at flash offset 0x%06x would overwrite "
                       "the flash-resident bootloader (< 0x%06x). Aborting.\n",
                       off, BOOTLOADER_TOP);
        leave_bootloader(); return 1;
    }
    if(off + h.len > FLASH_TOP){
        fprintf(stderr,"REFUSING: image ends at 0x%06x, past the HC32F460's "
                       "512 KiB of flash (last valid 0x%06x).\n",
                       off+h.len-1, FLASH_TOP-1);
        leave_bootloader(); return 1;
    }

    /* 4) program. */
    fprintf(stderr,"programming %u bytes in %u frames\n", h.len,
            (h.len + IAP_DATA_MAX - 1)/IAP_DATA_MAX);
    int ok = iap_flash(fd, h.data, h.len, verbose);
    fprintf(stderr, ok ? "STM32iap finished. STM32Write: success.\n"
                       : "STM32Write: failed.\n");

    /* 5) leave bootloader state. IAPCommand itself only drops GPIO 35 here;
     * the extra pulse is what makes the result observable without a second
     * tool run. */
    leave_bootloader();
    touch_path(rev==SYNC_REV_NA ? "/root/N32" : "/root/H32");
    fprintf(stderr,"STM32iap finished %s.\n", rev==SYNC_REV_NA ? "NA" : "HD");

    close(fd);
    free(h.data);
    return ok ? 0 : 1;
}
