/* ad4_idprobe.c — prove MCU *receive* path: repeatedly send the framed Klipper
 * identify (msgid 0x01, offset=0, count=64) and hexdump every reply byte with
 * timestamps. Frame parse + CRC check happens Mac-side on the captured dump.
 * Usage: ad4_idprobe <baud> <port> [wait_ms]
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

static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000LL + ts.tv_nsec/1000000LL;
}
static int open_raw(const char *port, int baud){
    int fd=open(port, O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){ fprintf(stderr,"open %s: %s\n",port,strerror(errno)); return -1; }
    struct termios t;
    if(tcgetattr(fd,&t)){ close(fd); return -1; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL|CREAD;
    t.c_cflag &= ~CSTOPB; t.c_cflag &= ~PARENB;
    t.c_cflag &= ~CSIZE;  t.c_cflag |= CS8;
    cfsetispeed(&t,(speed_t)baud); cfsetospeed(&t,(speed_t)baud);
    tcsetattr(fd,TCSANOW,&t);
    tcflush(fd,TCIOFLUSH);
    return fd;
}
int main(int argc,char**argv){
    int baud = argc>1?atoi(argv[1]):115200;
    const char *port = argc>2?argv[2]:"/dev/ttyS1";
    int wait_ms = argc>3?atoi(argv[3]):15000;
    int fd=open_raw(port,baud);
    if(fd<0) return 2;
    /* framed identify: len=8 seq=0x10 msgid=0x01 offset=0 count=64 crc b1 d1 term 7e */
    static const unsigned char ID[8] =
        {0x08,0x10,0x01,0x00,0x40,0xb1,0xd1,0x7e};
    long long t0=now_ms(), last_send=0;
    unsigned char buf[8192]; int total=0;
    while(now_ms()-t0 < wait_ms){
        long long now=now_ms();
        if(now-last_send >= 400){
            write(fd, ID, sizeof ID);
            last_send=now;
            fprintf(stderr,"-> identify @ +%lldms\n", now-t0);
        }
        int r=read(fd, buf+total, sizeof buf - total);
        if(r>0){
            fprintf(stderr,"<- %d B @ +%lldms\n", r, now_ms()-t0);
            for(int i=total-r;i<total;i++) printf("%02x ", buf[i]);
            printf("\n");
            total+=r;
            if(total>=(int)sizeof buf) break;
        } else {
            if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) break;
            usleep(2000);
        }
    }
    fprintf(stderr,"TOTAL %d bytes received.\n", total);
    return total>0?0:1;
}
