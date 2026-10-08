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
 *   stm32isp-ad4 [-r hd|na] <na.hex> <hd.hex>      (factory-style, auto-select)
 *   stm32isp-ad4 -r hd klipper.hex                 (force revision, one image)
 *   stm32isp-ad4 -r hd -v klipper.hex
 *
 * The standard STM32 ROM-bootloader transport (0x43 sync, erase 0x44,
 * program 0x46, read-back 0x11, get-pid 0x02, jump 0x08) is unchanged from
 * upstream stm32isp — that part is hardware-defined and does not depend on the
 * vendor.
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

/* ---- SoC GPIO strapping (the board-specific magic) -------------------- */
#define NRST_GPIO   34      /* SoC GPIO controlling MCU NRST  */
#define BOOT0_GPIO  35      /* SoC GPIO controlling MCU BOOT0 / ISP select */
#define NRST_US     15680   /* 15.68 ms, matches factory IAPCommand usleep */
#define GPIO_ROOT   "/sys/class/gpio"

/* ---- STM32 ROM bootloader protocol (upstream stm32isp, unchanged) ----- */
#define CMD_SYNC     0x43   /* 'C' */
#define CMD_GO       0x04
#define CMD_GET      0x02   /* get PID */
#define CMD_ERASE    0x44
#define CMD_WRITE    0x46
#define CMD_READ     0x11
#define CMD_JUMP     0x08
#define ACK_OK       0x79   /* 'Y' */
#define NACK         0x00

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
static void ser_wr(int fd, const unsigned char *b, int n){
    int o=0; while(o<n){ int w=write(fd,b+o,n-o); if(w<0) die("write"); o+=w; }
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

    /* 3) sync with retry (the generic tool stops here and times out) */
    unsigned char c = CMD_SYNC;
    int connected=0; int tries=60;
    while(tries--){
        tcflush(g_fd, TCIOFLUSH);
        ser_wr(g_fd,&c,1);
        unsigned char b=0;
        if(ser_rd(g_fd,&b,1,80)==1){
            if(b==ACK_OK){ connected=1; break; }
        }
        if(tries%5==0) nrst_pulse();
    }
    if(!connected){
        fprintf(stderr,"IAPCommand-style error: Can not connect to board, %s timed-out.\n",port);
        gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
        return 1;
    }
    if(verbose) fprintf(stderr,"Connected to board.\n");

    unsigned short pid=0; st_getpid(g_fd,&pid);
    if(verbose) fprintf(stderr,"PID = 0x%04x\n",pid);

    /* DUMP MODE: read the full flash (0x08000000 .. +256KB) and write it
     * out. Read-only: never erases or programs. This is the safety backup
     * of the IAP program at 0x0 and the app at 0xC000. */
    if(dump_bin){
        const uint32_t FLASH_BASE=0x08000000u, FLASH_LEN=0x40000u, CHUNK=0x400u;
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

    /* 4) pick image (this board is the HD/H32 revision; -r na for NA) */
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

    /* 5) erase (2 pages) + program */
    /* erase whole app region in 2KB pages from base up to end */
    uint32_t end = h.base + h.len;
    for(uint32_t a=h.base & ~0x7FFu; a<=end; a+=0x800){
        if(verbose) fprintf(stderr,"erase 0x%08x\n",a);
        if(!st_erase(g_fd,a)){ fprintf(stderr,"erase fail @0x%08x\n",a); return 1; }
    }
    /* program in 1KB chunks (ROM program max is a page; 1024 is safe) */
    const int CH=1024;
    for(uint32_t o=0;o<h.len;o+=CH){
        int n=(h.len-o)<CH?(int)(h.len-o):CH;
        if(!st_write(g_fd, h.base+o, h.data+o, n)){
            fprintf(stderr,"program fail @0x%08x\n",h.base+o); return 1;
        }
        if(verbose) fprintf(stderr,"wrote %u/%u\n", o+n, h.len);
    }

    /* 6) verify a spot check */
    {
        unsigned char rb[256];
        uint32_t va=h.base; int vn=256<h.len?256:(int)h.len;
        if(st_read(g_fd,va,rb,vn)){
            if(memcmp(rb,h.data,vn)!=0) fprintf(stderr,"WARNING: verify mismatch\n");
            else if(verbose) fprintf(stderr,"verify OK (%d bytes)\n",vn);
        }
    }

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
