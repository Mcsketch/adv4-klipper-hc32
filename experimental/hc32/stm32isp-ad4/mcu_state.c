/* mcu_state.c — disambiguate MCU serial state on a given port/baud.
 *   Test A: send 0x43 (ISP sync). Reply 0x06/0x07  => MCU is in ROM-ISP.
 *   Test B: send Klipper IDENTIFY frame. Reply bytes => Klipper app alive.
 *   Both silent => app not running (stub parked / dead / wrong baud).
 * Usage: mcu_state <baud> <port>
 * Cross-built: zig cc -target arm-linux-musleabihf -mcpu=cortex_a7+vfp3+neon+thumb2 -O2 -static
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
#include <stdint.h>

static long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000LL+t.tv_nsec/1000000LL; }

/* Baud table — standard rates only (all present in musl: B9600..B921600).
 * 250000/500000 have no musl B-constant on kernel 3.4.39 and no termios2;
 * the flashed Klipper app under test is the 115200 build, so probe at 115200. */
static int open_raw(const char *port,int baud){
    int fd=open(port,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){ fprintf(stderr,"open %s: %s\n",port,strerror(errno)); return -1; }
    struct termios t; tcgetattr(fd,&t);
    cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD; t.c_cflag&=~(CSTOPB|PARENB); t.c_cflag&=~CSIZE; t.c_cflag|=CS8;
    speed_t sp=B115200;
    switch(baud){case 9600:sp=B9600;break;case 19200:sp=B19200;break;case 38400:sp=B38400;break;
    case 57600:sp=B57600;break;case 115200:sp=B115200;break;case 230400:sp=B230400;break;
    case 460800:sp=B460800;break;case 921600:sp=B921600;break;default:sp=B115200;break;}
    cfsetispeed(&t,sp); cfsetospeed(&t,sp);
    if(tcsetattr(fd,TCSANOW,&t)<0){ fprintf(stderr,"tcsetattr: %s\n",strerror(errno)); close(fd); return -1; }
    tcflush(fd,TCIOFLUSH);
    return fd;
}
/* send `tx` (len n), collect replies for wait_ms, hexdump. returns byte count. */
static int xfer(int fd,const unsigned char *tx,int n,int wait_ms){
    tcflush(fd,TCIOFLUSH);
    write(fd,tx,n);
    unsigned char buf[8192]; int total=0; long t0=now_ms(); int first=1;
    while(now_ms()-t0<wait_ms){
        int r=read(fd,buf+total,sizeof buf-total);
        if(r>0){ if(first){ printf("  +%ldms: first %d B\n",now_ms()-t0,r); first=0; } total+=r;
            for(int i=0;i<r && total<=48;i++) printf("%02x ",buf[total-r+i]);
            printf("\n");
            if(total>=(int)sizeof buf) break;
        } else { if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) break; usleep(500); }
    }
    return total;
}
int main(int argc,char**argv){
    int baud = argc>1?atoi(argv[1]):250000;
    const char *port = argc>2?argv[2]:"/dev/ttyS1";
    int fd=open_raw(port,baud);
    if(fd<0) return 2;
    fprintf(stderr,"[A] ISP sync (0x43) @%d on %s:\n",baud,port);
    unsigned char SYNC[1]={0x43};
    int a=xfer(fd,SYNC,1,400);
    fprintf(stderr,"[B] Klipper IDENTIFY @%d on %s:\n",baud,port);
    static const unsigned char IDENT[11]={0x49,0x44,0x45,0x4e,0x54,0x49,0x46,0x59,0x00,0xb3,0x9f};
    int b=xfer(fd,IDENT,11,1800);
    if(b==0){
        /* retry a few times — 250k identify is intermittent */
        for(int i=0;i<3 && b==0;i++){ usleep(400000); b=xfer(fd,IDENT,11,1500); }
    }
    printf("\nRESULT: isp_sync=%d bytes, klipper=%d bytes  => ",a,b);
    if(a>=1 && b==0) printf("IN ISP (app did not start)\n");
    else if(b>=1)     printf("KLIPPER ALIVE (replied %d B)\n",b);
    else if(a==0 && b==0) printf("SILENT (dead / stub-parked / wrong baud)\n");
    else printf("MIXED/ambiguous\n");
    close(fd);
    return (a+b)?0:1;
}
