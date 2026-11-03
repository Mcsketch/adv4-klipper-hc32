/* safeprobe.c — READ-ONLY capture of the HC32F460 ROM-ISP handshake.
 * Only does: sync 0x43, then GET-PID 0x02. NO erase, NO write, NO jump.
 * Build (Mac, cross to SoC soft-float armv7l):
 *   zig cc -target arm-linux-musleabi -std=c11 -O2 -static -o safeprobe safeprobe.c
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>

static double T0=0;
static double now_s(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec*1e-9;}

static void wrf(const char*p,const char*v){int fd=open(p,2);if(fd>=0){write(fd,v,strlen(v));close(fd);}}
static void gexp(int n){char p[64],b[16];snprintf(p,64,"/sys/class/gpio/gpio%d/direction",n);
  if(access(p,0)){snprintf(p,64,"/sys/class/gpio/export");snprintf(b,16,"%d",n);wrf(p,b);}
  snprintf(p,64,"/sys/class/gpio/gpio%d/direction",n);wrf(p,"out");usleep(20000);}
static void gset(int n,int v){char p[64];snprintf(p,64,"/sys/class/gpio/gpio%d/value",n);wrf(p,v?"1":"0");}
#define NRST_GPIO 34
#define BOOT0_GPIO 35

/* read up to n bytes, logging each with a timestamp; stop after quiet_ms of silence */
static size_t rxlog(int fd, const char*tag, int n, int quiet_ms){
    size_t got=0;
    for(int i=0;i<n;i++){
        unsigned char b;
        fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf);
        struct timeval tv; tv.tv_sec=quiet_ms/1000; tv.tv_usec=(quiet_ms%1000)*1000;
        int r=select(fd+1,&rf,NULL,NULL,&tv);
        if(r>0){ ssize_t k=read(fd,&b,1); if(k==1){ printf("  [%s] RECV 0x%02X  (T+%.2f)\n",tag,b,now_s()-T0); got++; } }
        else break; /* quiet -> stop */
    }
    return got;
}

int main(int argc,char**argv){
    int mode = (argc>1)?atoi(argv[1]):1; /* 1=strap+sync+getpid, 2=getpid only, 3=dump idle */
    int fd=open("/dev/ttyS1",O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){perror("open ttyS1");return 2;}
    struct termios t; tcgetattr(fd,&t); cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD; t.c_cflag&=~CSTOPB; t.c_cflag&=~PARENB;
    t.c_cflag&=~CSIZE; t.c_cflag|=CS8;
    cfsetispeed(&t,B115200); cfsetospeed(&t,B115200);
    tcsetattr(fd,TCSANOW,&t); tcflush(fd,TCIOFLUSH);
    T0=now_s();

    if(mode==3){
        printf("mode3: dump 3s of idle bytes (no send):\n");
        for(int i=0;i<30;i++){
            unsigned char b; fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf);
            struct timeval tv; tv.tv_sec=0; tv.tv_usec=100000;
            if(select(fd+1,&rf,NULL,NULL,&tv)>0){ssize_t k=read(fd,&b,1); if(k==1)printf("  [idle] 0x%02X (T+%.2f)\n",b,now_s()-T0);}
        }
        printf("end idle dump\n"); close(fd); return 0;
    }

    if(mode==1){
        gexp(BOOT0_GPIO); gexp(NRST_GPIO); gset(BOOT0_GPIO,1); sleep(1);
        gset(NRST_GPIO,0); usleep(15680); gset(NRST_GPIO,1); usleep(15680);
        tcflush(fd,TCIOFLUSH);
        printf("=== STRAPPED INTO ROM-ISP; SYNC 0x43 ===\n");
        unsigned char c=0x43; write(fd,&c,1);
        rxlog(fd,"sync",8,400);
        printf("=== GET-PID 0x02 ===\n");
        tcflush(fd,TCIOFLUSH);
        usleep(20000);
        unsigned char g=0x02; write(fd,&g,1);
        rxlog(fd,"getpid",8,300);
        /* release BOOT0 so MCU boots the app normally (READ-ONLY, nothing written) */
        gset(BOOT0_GPIO,0); gset(NRST_GPIO,0); usleep(15680); gset(NRST_GPIO,1);
        close(fd); return 0;
    }
    printf("=== GET-PID 0x02 ===\n");
    tcflush(fd,TCIOFLUSH);
    usleep(20000);
    unsigned char g=0x02; write(fd,&g,1);
    rxlog(fd,"getpid",8,200);

    close(fd);
    return 0;
}
