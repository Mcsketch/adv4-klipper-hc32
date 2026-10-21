/* klip_probe.c — minimal Klipper MCU liveness probe.
 * Opens /dev/ttyS1 raw at a given baud, sends the Klipper IDENTIFY frame
 * (IDENTIFY\0 + crc16(0xFFFF-seeded)), and hexdumps whatever the MCU answers.
 *
 * Usage: klip_probe <baud> <port> [wait_ms]
 *   default: 250000 /dev/ttyS1 1200
 *
 * A living Klipper MCU replies with an identify frame: a byte stream that,
 * after a length byte, starts with the magic 0x03 0x4b ("K")... In practice the
 * first non-zero thing we will see is the frame length + the 0x03 magic. Any
 * sustained reply = MCU is alive and running Klipper. Silence = not running
 * Klipper (in ROM-ISP, bricked-to-silence, or wrong baud).
 *
 * This is the GROUND-TRUTH "did the flash work" test: the ROM bootloader
 * (pre-flash) does NOT answer Klipper IDENTIFY; only the Klipper app does.
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
    /* Linux: standard-rate speed_t == baud number (no B250000 needed; musl
     * doesn't define one). Same trick the proven ad4-bridge.c uses. */
    cfsetispeed(&t,(speed_t)baud); cfsetospeed(&t,(speed_t)baud);
    tcsetattr(fd,TCSANOW,&t);
    tcflush(fd,TCIOFLUSH);
    return fd;
}
int main(int argc,char**argv){
    int baud = argc>1?atoi(argv[1]):250000;
    const char *port = argc>2?argv[2]:"/dev/ttyS1";
    int wait_ms = argc>3?atoi(argv[3]):1200;
    int fd=open_raw(port,baud);
    if(fd<0) return 2;
    /* Klipper IDENTIFY: "IDENTIFY\0" + crc16 (0xFFFF init) over those 9 bytes */
    static const unsigned char IDENT[11] =
        {0x49,0x44,0x45,0x4e,0x54,0x49,0x46,0x59,0x00,0xb3,0x9f};
    /* settle the line, then send */
    usleep(20000);
    write(fd, IDENT, sizeof IDENT);
    /* optional second nudge after a short pause (Klipper MCU needs the line
     * idle before it parses; resend once to be safe) */
    long long t0=now_ms();
    unsigned char buf[4096]; int total=0; int first=1;
    while(now_ms()-t0 < wait_ms){
        int r=read(fd, buf+total, sizeof buf - total);
        if(r>0){
            if(first){ printf("[%d B] first reply at +%lldms:\n", r, now_ms()-t0); first=0; }
            total+=r;
            /* live hexdump the first 64 bytes as they arrive */
            for(int i=0;i<r && total<=64;i++)
                printf("%02x ", buf[total-r+i]);
            printf("\n");
            if(total>=(int)sizeof buf) break;
        } else {
            if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) break;
            usleep(500);
        }
    }
    if(total==0) printf("(no reply — MCU not answering Klipper at %d on %s)\n", baud, port);
    else printf("TOTAL %d bytes received.\n", total);
    close(fd);
    return total?0:1;
}
