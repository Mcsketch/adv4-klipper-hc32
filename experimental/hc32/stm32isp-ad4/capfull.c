/* capfull.c — capture the FULL Klipper identify response with timestamps.
 * Reveals total size + gaps (=> MCU reboot loop).
 * Build: zig cc -target arm-linux-musleabi -std=c11 -O2 -static -o capfull capfull.c
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

static double now_s(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec*1e-9;}

int main(int argc,char**argv){
    int secs = (argc>1)?atoi(argv[1]):8;
    int fd=open("/dev/ttyS1",O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){perror("open ttyS1");return 2;}
    struct termios t;tcgetattr(fd,&t);cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD;t.c_cflag&=~CSTOPB;t.c_cflag&=~PARENB;
    t.c_cflag&=~CSIZE;t.c_cflag|=CS8;
    cfsetispeed(&t,(speed_t)250000);cfsetospeed(&t,(speed_t)250000);
    tcsetattr(fd,TCSANOW,&t);tcflush(fd,TCIOFLUSH);
    usleep(100000); tcflush(fd,TCIOFLUSH);

    double t0=now_s();
    printf("sending identify magic at T+%.3f\n",now_s()-t0);
    unsigned char id[8]={0x08,0x10,0x01,0x00,0x40,0xB1,0xD1,0x7E};
    write(fd,id,8);
    fflush(stdout);

    static unsigned char big[1<<20]; size_t tot=0;
    double last=-1; double deadline=t0+secs;
    while(now_s()<deadline){
        fd_set rf;FD_ZERO(&rf);FD_SET(fd,&rf);
        struct timeval tv;tv.tv_sec=0;tv.tv_usec=50000;
        int r=select(fd+1,&rf,NULL,NULL,&tv);
        if(r>0){
            ssize_t n=read(fd,big+tot,(size_t)(1<<20)-tot);
            if(n>0){
                for(ssize_t i=0;i<n;i++){
                    double bt=now_s()-t0;
                    if(last<0)            printf("\n  [T+%.3f]  start",bt);
                    else if (bt-last>0.05)printf("\n  [T+%.3f]  GAP %.3fs",bt,bt-last);
                    printf(" %02X",big[tot+i]);
                    last=bt;
                }
                tot+=(size_t)n; fflush(stdout);
            }
        }
    }
    printf("\nTOTAL: %zu bytes captured in %ds window\n",(size_t)tot,secs);
    close(fd);
    return 0;
}
