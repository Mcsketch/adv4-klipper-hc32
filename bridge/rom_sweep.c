/*
 * rom_sweep — find the ROM-ISP baud rate of the AD4 HC32F460.
 *
 * The vendor IAPCommand connects fine; our fork (115200) does not. dmesg shows
 * the vendor's uart0 ending at 230400. This tool resets the MCU into ROM-ISP
 * (BOOT0=1 + NRST pulse) and, for each candidate baud, loops the 0x43 sync and
 * reports which rate gets the HC32 reply (0x06=HD / 0x07=NA) or STM32 0x79.
 *
 * Usage: rom_sweep
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
#include <sys/stat.h>

#define NRST_GPIO  34
#define BOOT0_GPIO 35
#define NRST_US    15680
#define GPIO_ROOT  "/sys/class/gpio"
#define PORT       "/dev/ttyS1"

static int g_exported[64] = {0};
static void die(const char*s){perror(s);exit(1);}
static void wr(const char*p,const char*v){int fd=open(p,O_WRONLY);if(fd<0)die(p);write(fd,v,strlen(v));close(fd);}
static void gpio_export(int n){
    char p[128],dp[128],b[16];
    snprintf(p,sizeof p,GPIO_ROOT"/export");
    snprintf(dp,sizeof dp,GPIO_ROOT"/gpio%d/direction",n);
    snprintf(b,sizeof b,"%d",n);
    if(access(dp,F_OK)!=0){int fd=open(p,O_WRONLY);if(fd<0)die("export");write(fd,b,strlen(b));close(fd);g_exported[n]=1;}
    int fd=open(dp,O_WRONLY); if(fd>=0){write(fd,"out",3);close(fd);}
    usleep(20000);
}
static void gpio_set(int n,int v){char p[128];snprintf(p,sizeof p,GPIO_ROOT"/gpio%d/value",n);wr(p,v?"1":"0");}
static void nrst_pulse(void){gpio_set(NRST_GPIO,0);usleep(NRST_US);gpio_set(NRST_GPIO,1);usleep(NRST_US);}

static int ser_open(int baud){
    int fd=open(PORT,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){fprintf(stderr,"open %s: %s\n",PORT,strerror(errno));return -1;}
    struct termios t; if(tcgetattr(fd,&t)){close(fd);return -1;}
    cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD;
    t.c_cflag&=~CSTOPB;t.c_cflag&=~PARENB;
    t.c_cflag&=~CSIZE;t.c_cflag|=CS8;
    cfsetispeed(&t,(speed_t)baud);cfsetospeed(&t,(speed_t)baud);
    tcsetattr(fd,TCSANOW,&t);tcflush(fd,TCIOFLUSH);
    return fd;
}
static int ser_rd(int fd,unsigned char*b,int n,int ms){
    int got=0;while(got<n&&ms-->=0){int r=read(fd,b+got,n-got);if(r>0){got+=r;continue;}if(r<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)break;usleep(1000);}
    return got;
}
static void ser_wr(int fd,const unsigned char*b,int n){int o=0;while(o<n){int w=write(fd,b+o,n-o);if(w<0)die("write");o+=w;}}

/* Reset into ROM-ISP, then try the 0x43 sync at this baud. Returns the reply
 * byte (0x06/0x07/0x79) or 0 if no sync. */
static int try_baud(int baud){
    gpio_set(BOOT0_GPIO,1); sleep(1);
    int fd=ser_open(baud); if(fd<0) return 0;
    nrst_pulse();
    unsigned char c=0x43; int tries=40; int reply=0;
    while(tries--){
        tcflush(fd,TCIOFLUSH);
        ser_wr(fd,&c,1);
        unsigned char b=0;
        if(ser_rd(fd,&b,1,100)==1){
            if(b==0x06||b==0x07||b==0x79){ reply=b; break; }
        }
        if(tries%5==0) nrst_pulse();
    }
    close(fd);
    /* release to normal boot */
    gpio_set(BOOT0_GPIO,0); nrst_pulse();
    return reply;
}

int main(void){
    if(geteuid()!=0){fprintf(stderr,"run as root\n");return 2;}
    gpio_export(BOOT0_GPIO); gpio_export(NRST_GPIO);
    const int bauds[] = {9600,19200,38400,57600,115200,230400,460800,921600};
    int nb = sizeof(bauds)/sizeof(bauds[0]);
    printf("=== ROM-ISP baud sweep (0x43 sync) ===\n");
    for(int i=0;i<nb;i++){
        int r=try_baud(bauds[i]);
        printf("  %7d baud : %s\n", bauds[i],
            r==0 ? "no sync" :
            r==0x07 ? "SYNC 0x07 (HD/H32)" :
            r==0x06 ? "SYNC 0x06 (NA/N32)" :
            "SYNC 0x79 (STM32)");
        if(r) { printf("  >>> ROM listens at %d baud\n", bauds[i]); }
    }
    gpio_set(BOOT0_GPIO,0); nrst_pulse();
    return 0;
}
