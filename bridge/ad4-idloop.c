#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <errno.h>
#include <time.h>

#ifndef B115200
#define B115200 0x1002
#endif

/* Klipper identify offset=0 count=40, seq=0: 08 10 01 00 28 + crc(5e 9f) + 7e */
static const unsigned char IDENTIFY[8] = {0x08,0x10,0x01,0x00,0x28,0x5e,0x9f,0x7e};

static void set_baud(int fd, int baud) {
    struct termios t; memset(&t,0,sizeof t);
    t.c_iflag = IGNBRK; t.c_cflag = CS8|CLOCAL|CREAD;
    t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
    cfsetispeed(&t,(baud==250000)?(speed_t)250000:(speed_t)115200);
    cfsetospeed(&t,(baud==250000)?(speed_t)250000:(speed_t)115200);
    tcsetattr(fd,TCSANOW,&t); tcflush(fd,TCIOFLUSH);
}

static size_t rx_wait(int fd, unsigned char *buf, size_t cap, long wait_ms) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0);
    long dl=wait_ms*1000L; size_t tot=0;
    while(1){
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC,&t1);
        long el=(t1.tv_sec-t0.tv_sec)*1000000L+(t1.tv_nsec-t0.tv_nsec)/1000L;
        if(el>=dl) break;
        fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf);
        struct timeval tv={0,30000};
        if(select(fd+1,&rf,NULL,NULL,&tv)>0){
            ssize_t n=read(fd,buf+tot,cap-tot);
            if(n>0) tot+=n;
        }
    }
    return tot;
}

/* Hex dump a byte range; returns printable ascii side-by-side */
static void hexdump(const char *label, const unsigned char *b, size_t n){
    printf("%s %zu bytes:\n", label, n);
    for(size_t i=0;i<n;i++){
        if(i%16==0) printf("  %04zx ",i);
        printf("%02X ",b[i]);
        if(i%16==15) printf("\n");
    }
    if(n%16) printf("\n");
}

int main(int argc,char**argv){
    int baud=(argc>2)?atoi(argv[2]):115200;
    int loops=(argc>3)?atoi(argv[3]):20;
    int wait_ms=(argc>4)?atoi(argv[4]):800;
    int fd=open("/dev/ttyS1",O_RDWR|O_NOCTTY);
    if(fd<0){ perror("open"); return 2; }
    set_baud(fd,baud); usleep(100000); tcflush(fd,TCIOFLUSH);
    fprintf(stderr,"[+] looped identify x%d @%d wait=%dms\n",loops,baud,wait_ms);
    size_t best=0; unsigned char bestbuf[4096]={0};
    int hits=0;
    for(int i=0;i<loops;i++){
        tcflush(fd,TCIOFLUSH);
        write(fd,IDENTIFY,8);
        unsigned char r[4096]; size_t n=rx_wait(fd,r,sizeof r,wait_ms);
        if(n>0){
            hits++;
            if(n>best){ memcpy(bestbuf,r,n); best=n; }
            if(n<=64){
                printf("  loop %02d: %zu B  ",i,n);
                for(size_t k=0;k<n&&k<48;k++) printf("%02X ",r[k]);
                printf("\n");
            } else {
                printf("  loop %02d: %zu B (full identify!)\n",i,n);
                hexdump("    ",r,n<=256?n:256);
            }
        } else {
            printf("  loop %02d: SILENT\n",i);
        }
        usleep(30000);
    }
    printf("\n=== RESULT: %d/%d replies, best=%zu B ===\n",hits,loops,best);
    if(best>64) hexdump("FULL IDENTIFY:",bestbuf,best<=512?best:512);
    close(fd); return 0;
}
