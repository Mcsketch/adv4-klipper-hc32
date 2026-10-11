/*
 * stm32isp-ad4 — flash the Flashforge Adventurer 4 MCU (HC32F460, HD or NA
 * revision) from the SoC (Sunxi V3S / sun8i) Linux, with the board's GPIO
 * strapping built in.
 *
 * This is a self-contained stm32isp fork. The ONLY additions over a generic
 * stm32isp are the SoC-side GPIO dance that a stock tool does not know about:
 *   - BOOT0 = SoC GPIO 35  (must be HIGH before + during the reset pulse)
 *   - NRST  = SoC GPIO 34  (15.68 ms LOW, then 15.68 ms HIGH)
 * Those two lines are wired from the SoC to the MCU; a generic stm32isp on
 * another board has no access to them and times out at sync.
 *
 * Source of the GPIO numbers / delays / sync: the factory "IAPCommand" binary
 * (an unstripped stm32isp fork, md5 919864f702cf3c07665234ab8de7896e) and the
 * factory control-2.3.0/run.sh. Confirmed strings in IAPCommand:
 *   /dev/ttyS1, /sys/class/gpio, "echo %d > %s/export", "echo %c > %s/gpio%u/value",
 *   "touch /root/H32", "touch /root/N32", "STM32iap finished HD.",
 *   "STM32iap finished NA.", "file name HD:/NA:", "115200".
 *   (H32 = HD revision, N32 = NA revision.)
 *
 * Flash map (HD): application at 0x0800C000. The ROM ISP lives at 0x00000000
 * and is never written, so ANY state is recoverable by re-entering ISP.
 * Klipper hex: arm-none-eabi-objcopy -I binary -O ihex --change-address=0xC000 \
 *                    klipper.bin klipper.hex
 *
 * Build (on the SoC, armv7l, root):
 *   gcc -O2 -o stm32isp-ad4 stm32isp-ad4.c
 * Usage:
 *   stm32isp-ad4 [-r hd|na] [-v] <na.hex> <hd.hex>   (factory-style; the
 *                                                   revision is DETECTED
 *                                                   from the ROM sync reply)
 *   stm32isp-ad4 -r hd klipper.hex                  (one image, forced rev)
 *   stm32isp-ad4 -d mcu-full-flash.bin              (read-only 256KB dump)
 *
 * ROM-ISP handshake (HC32F460, HC32-specific and NORMAL — not a bug):
 *   0x43 'C' sync  ->  ROM replies ONE byte: 0x06 (HD/H32) or 0x07 (NA/N32).
 * The tool accepts that reply as a successful connect (a plain-STM32 0x79
 * 'Y' reply is accepted too, for portability) and uses it as the revision
 * detector. The ROM must have just been reset into ISP (BOOT0 HIGH + NRST
 * pulse) or it stays silent; the sync loop re-pulses NRST automatically.
 *
 * The STM32-style sub-command transport (erase 0x44, program 0x46,
 * read-back 0x11, get-pid 0x02, jump 0x08) is unchanged from upstream
 * stm32isp — hardware-defined, vendor-independent.
 */

/* Feature-test macro: expose POSIX symbols (usleep, cfmakeraw, strcasecmp)
 * under musl AND glibc. Must precede ALL includes. */
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
#include <sys/stat.h>
#include <utime.h>
#include <poll.h>
#include <time.h>

/* ---- SoC GPIO strapping (the board-specific magic) -------------------- */
#define NRST_GPIO   34      /* SoC GPIO controlling MCU NRST  */
#define BOOT0_GPIO  35      /* SoC GPIO controlling MCU BOOT0 / ISP select */
#define NRST_US     15680   /* 15.68 ms, matches factory IAPCommand usleep */
#define GPIO_ROOT   "/sys/class/gpio"

/* ---- ROM-ISP protocol -------------------------------------------------
 * The HC32F460 ROM bootloader speaks a STM32-style transport
 * (0x43 sync, erase 0x44, program 0x46, read-back 0x11, get-pid 0x02,
 * jump 0x08), but its SYNC HANDSHAKE IS HC32-SPECIFIC:
 *   host -> mcu : 0x43
 *   mcu  -> host: 1 byte  -- 0x06 (HD revision) or 0x07 (NA revision)
 * This reply is NORMAL behaviour of this MCU (a plain STM32 would answer
 * 0x79 'Y'); it is NOT an error and must not be worked around. It doubles
 * as the revision detector, so the -r flag is only a hint/override.
 * NOTE: the ROM is quiet until it has just been reset into ISP (BOOT0 HIGH
 * + NRST pulse); if the reply never comes, the MCU is not in ISP state —
 * re-pulse NRST (the loop below does that automatically).
 */
#define CMD_SYNC     0x43   /* 'C' */
#define CMD_GO       0x04
#define CMD_GET      0x02   /* get PID */
#define CMD_ERASE    0x44
#define CMD_WRITE    0x46
#define CMD_READ     0x11
#define CMD_JUMP     0x08
#define ACK_OK       0x79   /* 'Y' — STM32-style ack (sub-commands) */
#define SYNC_REV_HD  0x07   /* HC32 sync reply: HD (H32) revision   */
#define SYNC_REV_NA  0x06   /* HC32 sync reply: NA (N32) revision   */
#define NACK         0x00
/* Vendor IAP frame-ack: the ROM replies 0x06 after each good 1030-byte
 * packet (strace-verified). Same value as the HD-sync byte; context disambiguates. */
#define IAP_FRAME_OK   0x06
#define IAP_CMD_DATA   0x01
#define IAP_CMD_FINAL  0x04
#define IAP_DATA_MAX   1024

/* HD application base (the only window we ever write). */
#define HD_BASE     0x0800C000u
/* NA revision uses a different app window; kept for the auto path. */
#define NA_BASE     0x08010000u

/* ---- tiny sysfs GPIO helpers ------------------------------------------ */
static int g_fd = -1;
static int g_exported[64] = {0};

static void die(const char *s){ perror(s); exit(1); }
static void wr(const char *p, const char *v){
    int fd = open(p, O_WRONLY); if(fd<0) die(p);
    write(fd, v, strlen(v)); close(fd);
}
static void gpio_export(int n){
    char p[128];
    snprintf(p,sizeof p, GPIO_ROOT "/export");
    char b[16]; snprintf(b,sizeof b,"%d",n);
    char dp[128]; snprintf(dp,sizeof dp, GPIO_ROOT "/gpio%d/direction",n);
    if(access(dp,F_OK)==0){ /* already exported: skip export, keep going */ }
    else {
        int fd=open(p,O_WRONLY); if(fd<0) die("export");
        if(write(fd,b,strlen(b))<0){ close(fd); die("export write"); }
        close(fd);
        g_exported[n]=1;
    }
    /* direction = out */
    int fd=open(dp,O_WRONLY);
    if(fd>=0){ write(fd,"out",3); close(fd); }
    usleep(20000);
}
static void gpio_set(int n, int v){
    char p[128];
    snprintf(p,sizeof p, GPIO_ROOT "/gpio%d/value", n);
    wr(p, v?"1":"0");
}
static void gpio_unexport(int n){
    if(g_exported[n]) {
        char p[128]; char b[16];
        snprintf(p,sizeof p, GPIO_ROOT "/unexport");
        snprintf(b,sizeof b,"%d",n);
        int fd=open(p,O_WRONLY); if(fd<0) return;
        write(fd,b,strlen(b)); close(fd);
    }
}

/* One reset pulse: NRST LOW 15.68ms, then HIGH 15.68ms. */
static void nrst_pulse(void){
    gpio_set(NRST_GPIO, 0); usleep(NRST_US);
    gpio_set(NRST_GPIO, 1); usleep(NRST_US);
}

/* ---- serial (115200 8N1, raw) ---------------------------------------- */
static int ser_open(const char *port, int baud){
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd<0){ fprintf(stderr,"open %s: %s\n",port,strerror(errno)); return -1; }
    struct termios t;
    if(tcgetattr(fd,&t)){ close(fd); return -1; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CSTOPB;           /* 1 stop bit */
    t.c_cflag &= ~PARENB;           /* no parity  */
    t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    (void)baud;
    return fd;
}
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
static int ser_wr(int fd, const unsigned char *b, int n){
    /* The port is O_NONBLOCK, so a burst of frames back-to-back can outrun the
     * UART TX buffer and write() returns EAGAIN (Resource temporarily
     * unavailable). The vendor tool's port is blocking, so its write() paces
     * at UART speed naturally; here we reproduce that pacing explicitly: on
     * EAGAIN, poll for POLLOUT (space in the TX FIFO) and retry. A per-write
     * deadline (10 s) guards against a wedged UART rather than hanging. */
    int o=0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while(o<n){
        int w = write(fd, b+o, n-o);
        if(w>0){ o+=w; continue; }
        if(w<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            poll(&pfd, 1, 50);   /* 50 ms slice */
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long used = (now.tv_sec - t0.tv_sec)*1000 + (now.tv_nsec - t0.tv_nsec)/1000000;
            if(used>10*1000){ errno=EAGAIN; return -1; }
            continue;
        }
        if(w<0) return -1;   /* real error */
        return o;
    }
    return o;
}
/* expect one ack byte, with a short retry window */
static int wait_ack(int fd, int expect){
    unsigned char b; int tries=300; /* ~300ms */
    while(tries--){
        if(ser_rd(fd,&b,1,5)==1){
            if(b==(unsigned char)expect) return 1;
            /* some ROMs send a couple of status bytes; keep going */
        }
    }
    return 0;
}

/* ---- STM32 ROM bootloader ops ----------------------------------------- */
static int st_getpid(int fd, unsigned short *pid){
    unsigned char c = CMD_GET; ser_wr(fd,&c,1);
    if(!wait_ack(fd, ACK_OK)) return 0;
    unsigned char b[8]; if(ser_rd(fd,b,4,300)<4) return 0;
    *pid = (b[0]<<8)|b[1];
    return 1;
}
static int st_erase(int fd, uint32_t addr){
    unsigned char c=CMD_ERASE; ser_wr(fd,&c,1);
    if(!wait_ack(fd,ACK_OK)) return 0;
    unsigned char a[4]={ (addr>>24)&0xff,(addr>>16)&0xff,(addr>>8)&0xff,addr&0xff };
    ser_wr(fd,a,4);
    return wait_ack(fd,ACK_OK);
}
static int st_write(int fd, uint32_t addr, const unsigned char *data, int len){
    unsigned char c=CMD_WRITE; ser_wr(fd,&c,1);
    if(!wait_ack(fd,ACK_OK)) return 0;
    unsigned char a[4]={ (addr>>24)&0xff,(addr>>16)&0xff,(addr>>8)&0xff,addr&0xff };
    ser_wr(fd,a,4);
    unsigned char l[2]={ (len>>8)&0xff, len&0xff };
    ser_wr(fd,l,2);
    if(!wait_ack(fd,ACK_OK)) return 0;
    ser_wr(fd,data,len);
    return wait_ack(fd,ACK_OK);
}
static int st_read(int fd, uint32_t addr, unsigned char *data, int len){
    unsigned char c=CMD_READ; ser_wr(fd,&c,1);
    if(!wait_ack(fd,ACK_OK)) return 0;
    unsigned char a[4]={ (addr>>24)&0xff,(addr>>16)&0xff,(addr>>8)&0xff,addr&0xff };
    ser_wr(fd,a,4);
    unsigned char l[2]={ (len>>8)&0xff, len&0xff };
    ser_wr(fd,l,2);
    if(!wait_ack(fd,ACK_OK)) return 0;
    return ser_rd(fd,data,len, len>256? (len/256+1)*30 : 300)==len;
}
static void st_jump(int fd){
    unsigned char c=CMD_JUMP; ser_wr(fd,&c,1);
    (void)wait_ack(fd,ACK_OK);
}

/* ---- vendor 1030-byte IAP packet protocol ---------------------------
 * Decoded from the factory IAPCommand (fillPacket + stm32Write). The HC32 ROM
 * does NOT speak the STM32 ROM-ISP commands (0x44/0x46/0x11); it speaks a
 * vendor packet:
 *   [cmd:1][len:2 LE][data:1024][crc:2 LE]
 *   crc = CRC16-CCITT (poly 0x1021, init 0, non-reflected, non-reversed) over
 *         the cmd byte + the 1024 data bytes (i.e. everything before the crc).
 * Commands (from IAPCommand): 0x01 = data chunk (ROM auto-erases the app pages
 * and writes), 0x04 = final "write complete" (exit bootloader).
 * Ack semantics (verified by strace of the vendor IAPCommand): after EACH
 * 1030-byte packet the ROM replies ONE byte 0x06 = frame-OK. (This is the same
 * byte as the HD-sync reply; the context — sync vs data phase — disambiguates.)
 * Silence = timeout = retry (up to 3). The ROM is self-addressing: it writes
 * each chunk at the next app-window address (auto-erasing the page), so the
 * hex file base is NOT sent on the wire — matching the factory tool, which
 * passes the same file to both the NA and HD slots and never transmits an
 * address. This is why there is NO separate erase command.
 */
static uint16_t crc16_iap(const unsigned char *p, int n){
    uint16_t c=0;
    for(int i=0;i<n;i++){
        c ^= (uint16_t)p[i] << 8;
        for(int b=0;b<8;b++)
            c = (c & 0x8000) ? (uint16_t)((c<<1) ^ 0x1021) : (uint16_t)(c<<1);
    }
    return c;
}
/* Fire one vendor data-frame (1030 bytes) — write-only, NO ack read.
 * On-wire layout — VERIFIED byte-for-byte against a live strace of the vendor
 * IAPCommand (28 frames, all CRCs match), and cross-checked against the
 * disassembled fillPacket/stm32Write:
 *   [0]        0x01 (data cmd)
 *   [1]        sequence number of this frame, 0-based (0,1,2,...,n-1)
 *   [2:3]      length, LITTLE-ENDIAN (byte count of this chunk, <=1024)
 *   [4:1027]   data, 1024 bytes, 0xFF-padded when the last chunk is short
 *   [1028:1029] crc16, LITTLE-ENDIAN, = CRC16-CCITT(poly 0x1021, init 0x0000,
 *                non-reflected, non-reversed) over the FULL 1024-byte data
 *                region INCLUDING the 0xFF padding.
 *
 * FIRE-AND-FORGET (strace ground truth): the vendor IAPCommand performs ZERO
 * read() calls in the data phase — it writes the sync, then ALL frames
 * back-to-back with no delay, then the final 0x04. The ROM never sends a
 * per-frame 0x06 that the host consumes; waiting for one (as an earlier
 * version of this tool did) blocks on an ack that the flow does not use and
 * aborts the write. So we write and move on; success is determined by the
 * post-reset Klipper identify on /dev/ttyS1, not by a per-frame ack.
 * Returns 1 if the write() syscalls succeed, 0 on an I/O error. */
static int iap_xmit(int fd, int cmd, unsigned char seq, const unsigned char *data, int dlen){
    if(cmd == IAP_CMD_FINAL){
        unsigned char b = IAP_CMD_FINAL;   /* 0x04, single byte, no read */
        return ser_wr(fd, &b, 1) == 1;
    }
    unsigned char pkt[1030];
    memset(pkt, 0xFF, sizeof pkt);          /* 0xFF padding (matches vendor memset(...,0xff));
                                               padding region is part of the CRC */
    pkt[0] = (unsigned char)cmd;            /* 0x01 */
    pkt[1] = seq;                           /* running frame sequence, 0-based */
    pkt[2] = (unsigned char)(dlen & 0xff);  /* length, little-endian */
    pkt[3] = (unsigned char)((dlen >> 8) & 0xff);
    memcpy(pkt + 4, data, dlen);            /* data at offset 4 */
    uint16_t crc = crc16_iap(pkt + 4, 1024); /* CRC over FULL 1024-byte region (incl. 0xFF pad) */
    pkt[1028] = (unsigned char)(crc & 0xff); /* crc, little-endian */
    pkt[1029] = (unsigned char)((crc >> 8) & 0xff);
    return ser_wr(fd, pkt, sizeof pkt) == (int)sizeof pkt;
}
/* Flash the whole image as vendor data-frames + final 0x04, back-to-back with
 * no inter-frame delay and no per-frame read — byte-for-byte the vendor
 * IAPCommand flow. [1] is the running frame sequence (0-based); the ROM is
 * self-addressing (auto-erase + auto-increment inside the app window), so no
 * address is sent. Returns 1 if every write() succeeded. */
static int iap_flash(int fd, uint32_t base, const unsigned char *data, uint32_t len){
    unsigned char seq=0;
    for(uint32_t o=0; o<len; o+=IAP_DATA_MAX){
        int n = (len-o)<IAP_DATA_MAX ? (int)(len-o) : IAP_DATA_MAX;
        if(!iap_xmit(fd, IAP_CMD_DATA, seq, data+o, n)){
            fprintf(stderr,"iap data frame write failed (seq=%d @0x%08x, %d bytes): %s\n",
                    seq, base+o, n, strerror(errno));
            return 0;
        }
        seq++;
    }
    if(!iap_xmit(fd, IAP_CMD_FINAL, 0, NULL, 0)){
        fprintf(stderr,"iap final (0x04 write-complete) failed\n");
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
static void touch_path(const char *p){ (void)open(p, O_CREAT|O_WRONLY, 0644); }

static void ihex_load(const char *path, ihex_t *h, uint32_t *err){
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"open %s: %s\n",path,strerror(errno)); *err=1; return; }
    uint32_t cap=1u<<20; uint8_t *buf=malloc(cap);
    uint32_t base=0;          /* running address base from type 04/05 records */
    uint32_t maxa=0, minabs=~0u; int have=0;
    char line[1024];
    while(fgets(line,sizeof line,f)){
        line[strcspn(line,"\r\n")]=0;
        if(line[0]!=':' || (int)strlen(line)<11){ continue; }
        int cnt=hexn(line+1,2);
        uint16_t off=hexn(line+3,4);
        int typ=hexn(line+7,2);
        if(typ==0x04){                 /* upper address */
            base=((uint32_t)hexn(line+9,4))<<16; continue;
        }
        if(typ==0x05){                 /* extended linear address */
            base=((uint32_t)hexn(line+9,4))<<16; continue;
        }
        if(typ==0x01){ break; }        /* EOF */
        if(typ!=0x00 || (int)strlen(line) < 5+cnt*2+5){ continue; }
        for(int i=0;i<cnt;i++){
            int byte=hexn(line+9+2*i,2);
            uint32_t abs=base+(uint32_t)off+i;
            if(!have || abs<minabs) minabs=abs;
            if(abs>=cap){ while(abs>=cap) cap<<=1; buf=realloc(buf,cap); }
            buf[abs-minabs]=(uint8_t)byte;
            if(abs>maxa) maxa=abs;
            have=1;
        }
    }
    fclose(f);
    if(!have){ free(buf); *err=1; return; }
    h->base=minabs; h->data=buf; h->len=maxa-minabs+1; *err=0;
}

/* ---- main -------------------------------------------------------------- */
int main(int argc, char **argv){
    const char *port = "/dev/ttyS1";
    const char *na_hex=NULL, *hd_hex=NULL, *dump_bin=NULL;
    int force_hd=-1;      /* -1 auto, 0 na, 1 hd */
    int verbose=0;
    int i=1;
    for(; i<argc; i++){
        if(!strcmp(argv[i],"-d")){
            i++; if(i>=argc) die("-d needs <out.bin>");
            dump_bin=argv[i];
        }
        else if(!strcmp(argv[i],"-r")){
            i++; if(i>=argc) die("-r needs hd|na");
            if(!strcasecmp(argv[i],"hd")) force_hd=1;
            else if(!strcasecmp(argv[i],"na")) force_hd=0;
            else die("rev is hd|na");
        } else if(!strcmp(argv[i],"-p")){ i++; port=argv[i]; }
        else if(!strcmp(argv[i],"-v")){ verbose=1; }
        else if(argv[i][0]=='-' && argv[i][1]) { /* unknown flag, skip */ }
        else {
            if(!na_hex) na_hex=argv[i];
            else if(!hd_hex) hd_hex=argv[i];
        }
    }
    if(!na_hex && !hd_hex && !dump_bin){
        fprintf(stderr,"usage: %s [-d <out.bin>] [-r hd|na] [-p /dev/ttyS1] [-v] [na.hex] [hd.hex]\n",argv[0]);
        fprintf(stderr,"       %s -d mcu-full-flash.bin      (read full 256KB flash, no write)\n",argv[0]);
        return 2;
    }
    if(geteuid()!=0){ fprintf(stderr,"must run as root (GPIO + /dev/ttyS1)\n"); return 2; }

    /* 1) GPIO strapping (match IAPCommand: serial open FIRST, then reset) */
    g_fd = ser_open(port,115200);
    if(g_fd<0) return 1;
    gpio_export(BOOT0_GPIO); gpio_export(NRST_GPIO);
    gpio_set(BOOT0_GPIO,1);           /* BOOT0 HIGH = select ROM ISP on reset */
    sleep(1);                          /* factory hold */
    nrst_pulse();                      /* put MCU into the ROM bootloader */

    /* 3) sync with retry.
     * The HC32 ROM answers 0x43 with ONE byte: 0x06 or 0x07 (revision id) —
     * normal behaviour, no further handshake needed. (A plain STM32 ROM
     * would answer 0x79 'Y'; we accept that too so the tool stays usable on
     * other boards.) The reply byte doubles as the revision detector, so
     * -r is only a hint/override. No extra wait beyond the reply: the ROM
     * is ready to take commands as soon as it has answered the sync. */
    unsigned char c = CMD_SYNC;
    int connected=0; unsigned char rev=0; int tries=60;
    while(tries--){
        tcflush(g_fd, TCIOFLUSH);
        ser_wr(g_fd,&c,1);
        unsigned char b=0;
        if(ser_rd(g_fd,&b,1,80)==1){
            if(b==SYNC_REV_HD || b==SYNC_REV_NA || b==ACK_OK){
                rev=b; connected=1; break;
            }
        }
        if(tries%5==0) nrst_pulse();
    }
    if(!connected){
        fprintf(stderr,"IAPCommand-style error: Can not connect to board, %s timed-out.\n",port);
        fprintf(stderr,"(the MCU answers the 0x43 sync with 0x06/0x07; silence means it is not in ROM-ISP state)\n");
        gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
        return 1;
    }
    if(verbose) fprintf(stderr,"Connected to board. (sync reply 0x%02x = %s)\n", rev,
                        rev==SYNC_REV_HD ? "HD/H32" :
                        rev==SYNC_REV_NA ? "NA/N32" : "generic STM32 ack");

    /* NOTE: we deliberately do NOT send a get-PID (0x02) here. The vendor
     * IAPCommand goes straight from sync to the first 0x01 data frame. An
     * extra 0x02 leaves stray reply bytes in the RX path that can desync the
     * per-frame 0x06 acks. (The HC32 ROM's get-PID is not part of the flash
     * flow anyway.) */

    /* DUMP MODE: read the full flash and write it out. Read-only: never
     * erases or programs. This is the safety backup of the IAP/ROM at 0x0
     * and the app at 0xC000.
     *
     * ADDRESS CONVENTION: the HC32F460 (HD) ROM-ISP uses RELATIVE addressing
     * -- the 4-byte address field is an offset from flash start, so 0x00000000
     * is the ROM-ISP/reset region and 0x0000C000 is the HD app window
     * (matching how the factory HD image and IAPCommand position it: type-04
     * base 0x0000 + offset 0xC000). The NA (N32) ROM uses absolute 0x08010000.
     * So the dump must start at 0x0 (NOT 0x08000000): reading at 0x08000000 is
     * out of range for the HD ROM and every chunk times out (hangs). Reading
     * 0x00000000..0x40000 covers the whole 256KB: ROM-ISP @0x0 + app @0xC000.
     * (The erase/program path already uses h.base=0xC000 for the same reason.) */
    if(dump_bin){
        const uint32_t FLASH_BASE=0x00000000u, FLASH_LEN=0x40000u, CHUNK=0x400u;
        uint8_t *buf=malloc(FLASH_LEN);
        if(!buf){ fprintf(stderr,"alloc failed\n"); return 1; }
        uint32_t fails=0;
        for(uint32_t a=0;a<FLASH_LEN;a+=CHUNK){
            if(!st_read(g_fd,FLASH_BASE+a,buf+a,CHUNK)){ fails++; }
            else if(verbose && (a%0x4000)==0) fprintf(stderr,"  read 0x%08X..+0x%03X ok\n",FLASH_BASE+a,CHUNK);
        }
        FILE *f=fopen(dump_bin,"wb");
        if(!f){ fprintf(stderr,"open %s: %s\n",dump_bin,strerror(errno)); free(buf); return 1; }
        fwrite(buf,1,FLASH_LEN,f); fclose(f); free(buf);
        fprintf(stderr,"DUMP: %s (%u bytes, %u chunk-fails) @0x08000000\n",dump_bin,FLASH_LEN,fails);
        /* leave MCU running the app (dump is read-only, nothing was changed) */
        st_jump(g_fd);
        gpio_set(BOOT0_GPIO,0);
        nrst_pulse();
        gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
        return (fails==0)?0:1;
    }

    /* 4) pick image. Revision is DETECTED from the sync reply (0x06=HD,
     * 0x07=NA); -r overrides only as an explicit hint. */
    if(rev==SYNC_REV_HD) force_hd=1;
    else if(rev==SYNC_REV_NA) force_hd=0;
    const char *img;
    if(force_hd==1){ img=hd_hex?hd_hex:na_hex; }
    else if(force_hd==0){ img=na_hex; }
    else { img=hd_hex?hd_hex:na_hex; }
    if(!img){ fprintf(stderr,"no image for revision\n"); return 1; }

    ihex_t h; uint32_t err=0; ihex_load(img,&h,&err);
    if(err){ fprintf(stderr,"failed to parse %s\n",img); return 1; }
    if(verbose) fprintf(stderr,"loaded %s: %u bytes @ 0x%08x .. 0x%08x\n",
                        img,h.len,h.base,h.base+h.len-1);

    /* SAFETY: the one thing we must never do is write the MCU's reset/boot
     * vector region at the very start of flash (0x00000000) — that is what
     * makes every state recoverable (re-enter ROM ISP). The factory HD image
     * lives at 0x0000C000 and the NA image at 0x08010000; both are well clear
     * of the vector region. Refuse anything that would touch it. */
    if(h.base < 0x00008000u){
        fprintf(stderr,"REFUSING: image base 0x%08x would overwrite the MCU "
                        "reset/ISP region (<= 0x00008000). Aborting.\n", h.base);
        gpio_set(BOOT0_GPIO,0); nrst_pulse();
        gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
        return 1;
    }

    /* 5) program via the vendor 1030-byte frame protocol.
     * The HC32 ROM is self-addressing from the app base and auto-erases each
     * page on write, so there is NO separate erase command and NO address is
     * sent on the wire. Each 1024-byte chunk is one 0x01 frame (ROM replies
     * 0x06); the image is closed with one 0x04 "write complete" frame. This
     * is byte-for-byte the vendor IAPCommand flow (verified by strace).
     *
     * NOTE: the ROM read-back op (STM32 0x11) is not spoken by this ROM and is
     * not yet decoded, so we cannot read flash back over ISP. Success is
     * indicated by the ROM's per-frame 0x06 acks — the same signal the vendor
     * tool uses. (Full-chip dump requires the HC32 read op; tracked as an
     * open item in adv4-iap-protocol.md.) */
    if(verbose) fprintf(stderr,"programming %u bytes via vendor 1030B frames (auto-erase, header carries addr)\n", h.len);
    if(!iap_flash(g_fd, h.base, h.data, h.len)){
        fprintf(stderr,"program failed (ROM stopped acking 0x06)\n");
        gpio_set(BOOT0_GPIO,0); nrst_pulse();
        gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
        return 1;
    }
    if(verbose) fprintf(stderr,"all frames acked 0x06 -> write complete\n");

    /* 7) exit ISP, run new app */
    st_jump(g_fd);
    gpio_set(BOOT0_GPIO,0);            /* release BOOT0 -> normal app boot */
    nrst_pulse();

    /* 8) record outcome like the factory tool (H32=HD, N32=NA) */
    if(force_hd==0 || h.base>=NA_BASE){ touch_path("/root/N32"); fprintf(stderr,"STM32iap finished NA.\n"); }
    else                              { touch_path("/root/H32"); fprintf(stderr,"STM32iap finished HD.\n"); }

    close(g_fd);
    gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
    return 0;
}
