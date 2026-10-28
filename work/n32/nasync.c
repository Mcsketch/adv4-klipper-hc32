/* nasync.c — READ-ONLY sync capture for the AD4 SoC<->MCU link (NA / N32 unit).
 *
 * Does exactly what IAPCommand does to get the MCU talking — GPIO 35 high,
 * NRST (GPIO 34) low 15.68 ms / high 15.68 ms — then sends the single sync
 * byte 0x43 and logs EVERY byte that comes back, with timestamps, for up to
 * one quiet second. Nothing else is ever written to the port: no erase, no
 * frames, no commands. At the end GPIO 35 goes low and NRST is pulsed again
 * so the MCU boots its normal application.
 *
 *   nasync [-p /dev/ttyS1] [-n tries] [-b 0|1] [-q quiet_ms]
 *     -b  level of GPIO 35 during the reset (default 1 = IAPCommand's strap)
 *     -n  sync attempts (default 6); NRST is re-pulsed before each one
 *
 * Build on prodesk:  arm-linux-gnueabi-gcc -static -O2 -std=gnu11 -o nasync nasync.c
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
#include <time.h>
#include <sys/select.h>

#define NRST_GPIO 34
#define FLAG_GPIO 35
#define NRST_US   15680

static double t0;
static double now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }

static void wrf(const char *p, const char *v){
    int fd=open(p,O_WRONLY); if(fd<0){ fprintf(stderr,"open %s: %s\n",p,strerror(errno)); return; }
    if(write(fd,v,strlen(v))<0) fprintf(stderr,"write %s: %s\n",p,strerror(errno));
    close(fd);
}
static void gexport(int n){
    char p[64], b[16];
    snprintf(p,sizeof p,"/sys/class/gpio/gpio%d/direction",n);
    if(access(p,F_OK)){ snprintf(b,sizeof b,"%d",n); wrf("/sys/class/gpio/export",b); usleep(20000); }
    wrf(p,"out"); usleep(20000);
}
static void gunexport(int n){ char b[16]; snprintf(b,sizeof b,"%d",n); wrf("/sys/class/gpio/unexport",b); }
static void gset(int n, int v){ char p[64]; snprintf(p,sizeof p,"/sys/class/gpio/gpio%d/value",n); wrf(p,v?"1":"0"); }
static void nrst_pulse(void){ gset(NRST_GPIO,0); usleep(NRST_US); gset(NRST_GPIO,1); usleep(NRST_US); }

static int ser_open(const char *port){
    int fd=open(port,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){ fprintf(stderr,"open %s: %s\n",port,strerror(errno)); return -1; }
    struct termios t; if(tcgetattr(fd,&t)){ perror("tcgetattr"); return -1; }
    cfmakeraw(&t); cfsetispeed(&t,B115200); cfsetospeed(&t,B115200);
    t.c_cflag |= CLOCAL|CREAD; t.c_cflag &= ~(CRTSCTS|PARENB|CSTOPB); t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
    if(tcsetattr(fd,TCSANOW,&t)){ perror("tcsetattr"); return -1; }
    tcflush(fd,TCIOFLUSH);
    return fd;
}

/* log bytes until quiet_ms of silence or 64 bytes; returns count */
static int rxlog(int fd, const char *tag, int quiet_ms){
    int got=0;
    for(;;){
        fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf);
        struct timeval tv={ quiet_ms/1000, (quiet_ms%1000)*1000 };
        int r=select(fd+1,&rf,NULL,NULL,&tv);
        if(r<=0) break;
        unsigned char b; ssize_t k=read(fd,&b,1);
        if(k!=1) break;
        printf("  [%s] RECV 0x%02X %s (T+%.3f)\n",tag,b,(b>=0x20&&b<0x7f)?(char[]){'\'',(char)b,'\'',0}:"   ",now()-t0);
        if(++got>=64) break;
    }
    return got;
}

int main(int argc, char **argv){
    const char *port="/dev/ttyS1"; int tries=6, flag=1, quiet=1000;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-p")&&i+1<argc) port=argv[++i];
        else if(!strcmp(argv[i],"-n")&&i+1<argc) tries=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-b")&&i+1<argc) flag=atoi(argv[++i])?1:0;
        else if(!strcmp(argv[i],"-q")&&i+1<argc) quiet=atoi(argv[++i]);
        else { fprintf(stderr,"usage: %s [-p port] [-n tries] [-b 0|1] [-q quiet_ms]\n",argv[0]); return 2; }
    }
    if(geteuid()!=0){ fprintf(stderr,"must run as root\n"); return 2; }
    t0=now();
    int fd=ser_open(port); if(fd<0) return 1;
    printf("port %s @115200 8N1 raw; GPIO%d=%d during reset; %d tries, %d ms quiet window\n",port,FLAG_GPIO,flag,tries,quiet);
    gexport(FLAG_GPIO); gexport(NRST_GPIO);
    gset(FLAG_GPIO,flag); sleep(1);
    int total=0;
    for(int i=0;i<tries;i++){
        tcflush(fd,TCIOFLUSH);
        nrst_pulse();
        printf("try %d: NRST pulsed (T+%.3f); listening before sync...\n",i+1,now()-t0);
        total+=rxlog(fd,"boot",300);
        unsigned char c=0x43;
        if(write(fd,&c,1)!=1){ perror("write 0x43"); break; }
        printf("try %d: sent 0x43 (T+%.3f)\n",i+1,now()-t0);
        int n=rxlog(fd,"sync",quiet); total+=n;
        if(!n) printf("try %d: silence\n",i+1);
    }
    printf("releasing: GPIO%d=0, NRST pulse -> normal boot\n",FLAG_GPIO);
    gset(FLAG_GPIO,0); nrst_pulse();
    gunexport(FLAG_GPIO); gunexport(NRST_GPIO);
    close(fd);
    printf("done: %d byte(s) received in total\n",total);
    return total?0:1;
}
