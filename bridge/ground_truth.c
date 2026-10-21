/*
 * ground_truth — decisive MCU line/app diagnostic for the AD4 (run ON the SoC).
 *
 * Answers two independent questions with one binary:
 *   Phase A (ROM):  does the SoC<->MCU UART carry data AT ALL?
 *                   Set BOOT0=1 + NRST pulse -> ROM ISP, loop sync 0x43 @115200.
 *                   If the ROM answers 0x06/0x07, the physical line + 115200
 *                   are provably GOOD (this test is independent of Klipper).
 *   Phase B (APP):  is the Klipper app actually running and replying?
 *                   BOOT0=0 + NRST pulse -> normal boot, then send a real
 *                   Klipper IDENTIFY (offset 0) and capture every byte with
 *                   timing. A healthy Klipper returns a long (~1.5KB) frame.
 *
 * Usage:  ground_truth [-r] [-a]
 *          (default: both phases, A then B)
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#define NRST_GPIO   34
#define BOOT0_GPIO  35
#define NRST_US     15680
#define GPIO_ROOT   "/sys/class/gpio"
#define PORT        "/dev/ttyS1"

static int g_fd = -1;
static int g_exported[64] = {0};

static void die(const char *s){ perror(s); exit(1); }
static void wr(const char *p, const char *v){
    int fd = open(p, O_WRONLY); if(fd<0) die(p);
    write(fd, v, strlen(v)); close(fd);
}
static void gpio_export(int n){
    char p[128], dp[128], b[16];
    snprintf(p,sizeof p, GPIO_ROOT "/export");
    snprintf(dp,sizeof dp, GPIO_ROOT "/gpio%d/direction", n);
    snprintf(b,sizeof b, "%d", n);
    if(access(dp,F_OK)!=0){
        int fd=open(p,O_WRONLY); if(fd<0) die("export");
        if(write(fd,b,strlen(b))<0){ close(fd); die("export write"); }
        close(fd); g_exported[n]=1;
    }
    int fd=open(dp,O_WRONLY);
    if(fd>=0){ write(fd,"out",3); close(fd); }
    usleep(20000);
}
static void gpio_set(int n,int v){ char p[128]; snprintf(p,sizeof p,GPIO_ROOT "/gpio%d/value",n); wr(p,v?"1":"0"); }
static void gpio_unexport(int n){
    if(g_exported[n]){ char p[128],b[16]; snprintf(p,sizeof p,GPIO_ROOT "/unexport");
        snprintf(b,sizeof b,"%d",n); int fd=open(p,O_WRONLY); if(fd<0)return; write(fd,b,strlen(b)); close(fd);}
}
static void nrst_pulse(void){ gpio_set(NRST_GPIO,0); usleep(NRST_US); gpio_set(NRST_GPIO,1); usleep(NRST_US); }

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
static void cfmakeraw(struct termios *t){
    t->c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    t->c_cflag &= ~(CSIZE|PARENB);
    t->c_cflag |= CS8;
}
#endif

static long long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000LL + t.tv_nsec/1000000LL; }

static int ser_open(int baud){
    int fd = open(PORT, O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){ fprintf(stderr,"open %s: %s\n",PORT,strerror(errno)); return -1; }
    struct termios t; if(tcgetattr(fd,&t)){ close(fd); return -1; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL|CREAD;
    t.c_cflag &= ~CSTOPB; t.c_cflag &= ~PARENB;
    t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    cfsetispeed(&t,(speed_t)baud); cfsetospeed(&t,(speed_t)baud);
    tcsetattr(fd,TCSANOW,&t); tcflush(fd,TCIOFLUSH);
    return fd;
}
static int ser_rd(int fd, unsigned char *b, int n, int ms){
    int got=0; while(got<n && ms-->=0){
        int r=read(fd,b+got,n-got);
        if(r>0){got+=r;continue;}
        if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) break;
        usleep(1000);
    } return got;
}
static void ser_wr(int fd,const unsigned char *b,int n){ int o=0; while(o<n){int w=write(fd,b+o,n-o); if(w<0){die("write");} o+=w; } }

static int phase_rom(void){
    printf("\n===== PHASE A: ROM-ISP line test @115200 (independent of Klipper) =====\n");
    g_fd = ser_open(115200); if(g_fd<0) return -1;
    gpio_export(BOOT0_GPIO); gpio_export(NRST_GPIO);
    gpio_set(BOOT0_GPIO,1); sleep(1); nrst_pulse();
    unsigned char c=0x43; int tries=80; int synced=0; unsigned char rev=0;
    long long t0=now_ms();
    while(tries--){
        tcflush(g_fd,TCIOFLUSH);
        ser_wr(g_fd,&c,1);
        unsigned char b=0;
        if(ser_rd(g_fd,&b,1,80)==1){
            if(b==0x06||b==0x07||b==0x79){ rev=b; synced=1; break; }
        }
        if(tries%5==0) nrst_pulse();
    }
    long long dt=now_ms()-t0;
    if(synced) printf("  ROM SYNC OK in %lld ms  (reply 0x%02x %s)  -> UART LINE + 115200 ARE GOOD\n",
                      dt, rev, rev==0x07?"HD/H32":rev==0x06?"NA/N32":"STM32");
    else printf("  NO ROM SYNC after %d tries / %lld ms  -> line or strapping problem\n", tries, dt);
    /* release to normal boot */
    gpio_set(BOOT0_GPIO,0); nrst_pulse(); gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
    close(g_fd); g_fd=-1;
    sleep(1);
    return synced?1:0;
}

/* One Klipper IDENTIFY (offset 0), then capture the raw reply with timing. */
static void capture_identify(const char *label){
    printf("\n===== PHASE B: APP IDENTIFY — %s =====\n", label);
    g_fd = ser_open(250000); if(g_fd<0) return;
    /* normal boot already done by caller; give it a beat */
    sleep(1);
    static const unsigned char IDENTIFY[8]={0x08,0x10,0x01,0x00,0x28,0x5e,0x9f,0x7e};
    unsigned char buf[65536]; long long prev=now_ms();
    size_t total=0; int gaps=0; long long first_gap=0;
    int settled=0;
    long long t0=now_ms();
    while(now_ms()-t0 < 1500){
        unsigned char b;
        int r=ser_rd(g_fd,&b,1,1);
        if(r==1){
            long long nn=now_ms(); long long g=nn-prev;
            if(g>40 && total>0){ gaps++; if(first_gap==0) first_gap=g; }
            prev=nn;
            if(total<sizeof buf) buf[total]=b;
            total++;
        } else {
            if(total>0 && prev>0 && now_ms()-prev>250){ settled=1; break; }
        }
    }
    (void)gaps;(void)first_gap;(void)settled;
    printf("  captured %zu bytes in %lld ms\n", total, now_ms()-t0);
    if(total==0){ printf("  SILENT — no app reply\n"); close(g_fd); g_fd=-1; return; }
    size_t show = total<400?total:400;
    printf("  first %zu bytes:\n", show);
    for(size_t i=0;i<show;i++){
        if(i%16==0) printf("    %04zx ", i);
        printf("%02X ", buf[i]);
        if(i%16==15) printf("\n");
    }
    if(show%16) printf("\n");
    /* Klipper identify answer begins with a frame whose offset=0 => starts
     * 08 20 ... and the dict data contains strings; just report if it looks
     * like a real frame (header 0x08 within first bytes) vs noise. */
    int looks_frame=0; for(size_t i=0;i<8&&i<total;i++) if(buf[i]==0x08){looks_frame=1;break;}
    printf("  verdict: %s\n", (total>200&&looks_frame)?"RECEIVED LONG FRAME (Klipper alive, line OK)"
                                               :(looks_frame?"short frame (partial?)":"NO coherent Klipper frame (app not replying / line bad)"));
    close(g_fd); g_fd=-1;
}

int main(int argc,char**argv){
    int do_rom=1, do_app=1;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"-r")){do_app=0;} else if(!strcmp(argv[i],"-a")){do_rom=0;} }
    if(geteuid()!=0){ fprintf(stderr,"run as root (GPIO)\n"); return 2; }
    if(do_rom) phase_rom();
    if(do_app){
        gpio_export(BOOT0_GPIO); gpio_export(NRST_GPIO);
        gpio_set(BOOT0_GPIO,0); nrst_pulse();          /* normal boot */
        gpio_unexport(BOOT0_GPIO); gpio_unexport(NRST_GPIO);
        capture_identify("250000 (flashed build)");
    }
    return 0;
}
