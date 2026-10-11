/* uart_rate.c — read the SoC UART's ACTUAL configured rate (ground truth).
 * TIOCGSERIAL (0x541C) -> struct serial_struct (56 bytes, kernel layout).
 * Prints baud_base / custom_divisor / uartclk so we can see whether a
 * 250000 request was honored or clamped. Read-only. Usage: uart_rate <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#ifndef TIOCGSERIAL
#define TIOCGSERIAL 0x541C
#endif
struct serial_struct {            /* must be 56 bytes (kernel) */
    int type; int line; unsigned int port; int irq; int flags;
    int xmit_fifo_size; int custom_divisor; int baud_base;
    unsigned short close_delay; char io_type; char mem_map_size;
    int hub6; unsigned int mapbase; unsigned short mapsize;
    unsigned short uartclk; unsigned char reserved[12]; unsigned int reserved1[2];
};
int main(int argc,char**argv){
    const char *port = argc>1?argv[1]:"/dev/ttyS1";
    if(sizeof(struct serial_struct)!=56) fprintf(stderr,"WARN: sizeof= %zu\n",sizeof(struct serial_struct));
    int fd=open(port,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){ fprintf(stderr,"open %s: %s\n",port,strerror(errno)); return 2; }
    struct serial_struct s; memset(&s,0,sizeof s);
    if(ioctl(fd,TIOCGSERIAL,&s)){ fprintf(stderr,"TIOCGSERIAL: %s\n",strerror(errno)); close(fd); return 1; }
    printf("port=%s line=%d flags=%#x\n", port, s.line, s.flags);
    printf("  baud_base=%d  custom_divisor=%d  uartclk(Hz)=%u\n", s.baud_base, s.custom_divisor, s.uartclk);
    unsigned eff = s.custom_divisor ? (unsigned)(s.uartclk/(8u*s.custom_divisor)) : (unsigned)s.baud_base;
    printf("  -> effective UART rate ~= %u Hz\n", eff);
    close(fd);
    return 0;
}
