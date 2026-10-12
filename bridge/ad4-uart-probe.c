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
#include <ctype.h>

#ifndef B250000
#define B250000 0x1003
#endif
#ifndef B115200
#define B115200 0x1002
#endif

static const unsigned char IDENTIFY[8] = {0x08,0x10,0x01,0x00,0x40,0xB1,0xD1,0x7E};

static void set_baud(int fd, int baud) {
    struct termios t;
    speed_t sp;
    memset(&t,0,sizeof t);
    t.c_iflag = IGNBRK;
    t.c_cflag = CS8|CLOCAL|CREAD;
    t.c_lflag=0; t.c_oflag=0;
    t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
    sp = (baud==115200)?B115200:B250000;
    cfsetispeed(&t,sp); cfsetospeed(&t,sp);
    tcsetattr(fd,TCSANOW,&t);
    tcflush(fd,TCIOFLUSH);
}

static size_t rx_until_deadline(int fd, unsigned char *buf, size_t cap, long wait_ms) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0);
    long dl = wait_ms*1000L; size_t tot=0;
    while (1){
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC,&t1);
        long el=(t1.tv_sec-t0.tv_sec)*1000000L+(t1.tv_nsec-t0.tv_nsec)/1000L;
        if (el>=dl) break;
        fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf);
        struct timeval tv; tv.tv_sec=0; tv.tv_usec=80000;
        int r=select(fd+1,&rf,NULL,NULL,&tv);
        if (r>0){
            ssize_t n=read(fd,buf+tot,cap-tot);
            if (n>0) tot+=n;
        }
    }
    return tot;
}

int main(int argc,char**argv){
    const char*mode = (argc>1)?argv[1]:"sync";
    const char*dev = (argc>2)?argv[2]:"/dev/ttyS1";
    int baud = (argc>3)?atoi(argv[3]):250000;
    int wait_ms= (argc>4)?atoi(argv[4]):1200;

    int fd=open(dev,O_RDWR|O_NOCTTY);
    if (fd<0){ fprintf(stderr,"open %s: %s\n",dev,strerror(errno)); return 2; }
    set_baud(fd,baud);
    usleep(80000);
    tcflush(fd,TCIOFLUSH);

    if (strcmp(mode,"sync")==0){
        unsigned char c=0x43;
        fprintf(stderr,"[+] sync 0x43 -> %s @%d\n",dev,baud);
        write(fd,&c,1);
        unsigned char r[64]; size_t n=rx_until_deadline(fd,r,64,wait_ms);
        if (n==0){ printf("NO_REPLY (MCU not in ROM-ISP; either running app-silent or dead)\n"); }
        else {
            printf("ROM-ISP-ALIVE: got %zu byte(s):",n);
            for (size_t i=0;i<n;i++) printf(" %02X",r[i]);
            printf("\n");
            if (n>=1 && r[0]==0x07) printf("=> 0x07 = ROM-ISP handshake. MCU is in BOOTLOADER, not app.\n");
        }
    } else {
        fprintf(stderr,"[+] identify (8B) -> %s @%d\n",dev,baud);
        write(fd,IDENTIFY,8);
        unsigned char r[8192]; size_t n=rx_until_deadline(fd,r,8192,wait_ms);
        if (n==0){ printf("SILENT\n"); }
        else {
            printf("IDENTIFY-REPLY %zu bytes:\n",n);
            for (size_t i=0;i<n && i<512;i++){
                if (i%16==0) printf("%04zx ",i);
                printf("%02X ",r[i]);
                if (i%16==15) printf("\n");
            }
            printf("\n");
        }
    }
    close(fd);
    return 0;
}
