/* bootloop.c — send identify every 3s for N seconds, log each response's
 * size + exact timing. A BOOTLOOP shows as: response, then a gap > 3s with
 * a "fresh" response (MCU rebooted between). STABLE shows responses every
 * 3s with consistent sizes.
 * Build: zig cc -target arm-linux-musleabi -std=c11 -O2 -static -o bootloop bootloop.c
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
    int rounds=(argc>1)?atoi(argv[1]):10;   /* 10 rounds x 3s = 30s */
    int fd=open("/dev/ttyS1",O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){perror("open");return 2;}
    struct termios t;tcgetattr(fd,&t);cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD;t.c_cflag&=~CSTOPB;t.c_cflag&=~PARENB;
    t.c_cflag&=~CSIZE;t.c_cflag|=CS8;
    cfsetispeed(&t,(speed_t)250000);cfsetospeed(&t,(speed_t)250000);
    tcsetattr(fd,TCSANOW,&t);
    double t0=now_s();
    unsigned char id[8]={0x08,0x10,0x01,0x00,0x40,0xB1,0xD1,0x7E};
    for(int r=0;r<rounds;r++){
        tcflush(fd,TCIOFLUSH);
        write(fd,id,8);
        /* collect up to 4s */
        size_t tot=0; double wait_deadline=now_s()+4.0; double first=-1;
        while(now_s()<wait_deadline){
            fd_set rf;FD_ZERO(&rf);FD_SET(fd,&rf);
            struct timeval tv;tv.tv_sec=0;tv.tv_usec=40000;
            int sel=select(fd+1,&rf,NULL,NULL,&tv);
            if(sel>0){
                unsigned char buf[4096]; ssize_t n=read(fd,buf,4096);
                if(n>0){ if(first<0)first=now_s()-t0; tot+=n; }
            }
        }
        double resp_t=(first<0)?-1:(first-t0);
        printf("round %2d: %-4zu bytes  %s\n", r, tot,
               tot==0 ? "(silent)" : (first>0 ? "responded" : ""));
        fflush(stdout);
    }
    close(fd); return 0;
}
