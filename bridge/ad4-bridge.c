/*
 * ad4-bridge — Adventurer 4 MCU serial<->TCP bridge, static ARM binary.
 *
 * The SoC (Sunxi V3S, kernel 3.4.39, busybox, NO python3) is factory-wired to
 * the printer MCU: MCU UART <-> SoC /dev/ttyS1. This program pumps bytes
 * between a TCP port and that serial line so a Klipper host on the other
 * end of Ethernet (here: a Mac) can drive the MCU with zero soldering and
 * zero Python:
 *
 *     Mac (klippy) -- /dev/adv4_mcu (socat PTY) -- ethernet --
 *     SoC (this: TCP :5000) -- /dev/ttyS1 @115200 8N1 raw -- MCU (Klipper)
 *
 * Topology notes:
 *   - TCP, single client at a time (Klipper). On client disconnect the
 *     serial fd is closed; the next accept re-opens the UART fresh
 *     (Klipper FIRMWARE_RESTART just reconnects).
 *   - Byte-transparent: cfmakeraw + CS8 + CLOCAL|CREAD, no echo, no CR/LF
 *     translation, tcflush(TCIOFLUSH) before hand-off.
 *   - 115200 is the rate that ACTUALLY works on this SoC: uart0 runs from a
 *     24 MHz clock, below the serial driver's [30 MHz, 120 MHz] setbaud
 *     range, so 250000 requests are rejected (dmesg sw_uart_check_baudset)
 *     and the line holds its boot rate. 115200 is a standard speed_t value
 *     (musl has no B250000 constant either way). The Klipper fw for the AD4
 *     must be built with SERIAL_BAUD=115200 to match.
 *   - The serial fd is non-blocking; EAGAIN is normal and handled.
 *   - Logging to stderr (nohup it to a file).
 *
 * Usage:  ad4-bridge [port] [uart]     (defaults 5000, /dev/ttyS1)
 * Run as root (serial ownership).
 *
 * Build (macOS, zig cross-compile, static, no FPU):
 *   zig cc -target arm-linux-musleabi -std=c11 -O2 -static \
 *        -o ad4-bridge ad4-bridge.c
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* cfmakeraw is provided by glibc and musl; only macOS/BSD lack it. */
#if defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
static void cfmakeraw(struct termios *t){
    t->c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    t->c_cflag &= ~(CSIZE|PARENB);
    t->c_cflag |= CS8;
}
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int  g_port   = 5000;
static char g_uart[128] = "/dev/ttyS1";
static int  g_baud   = 115200;   /* AD4 uart0 is 24 MHz: 250000 not programmable */
static int  g_verbose = 0;

static void logmsg(const char *m){
    if (g_verbose) { fprintf(stderr, "%s\n", m); fflush(stderr); }
}

/* Open the MCU UART: raw, 8N1, CLOCAL|CREAD, no echo, baud via raw value. */
static int uart_open(int baud){
    int fd = open(g_uart, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", g_uart, strerror(errno)); return -1; }
    struct termios t;
    if (tcgetattr(fd, &t) < 0) { close(fd); return -1; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CSTOPB;              /* 1 stop bit  */
    t.c_cflag &= ~PARENB;              /* no parity   */
    t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    /* Linux: standard-rate speed_t == baud number (no B250000 needed). */
    cfsetispeed(&t, (speed_t)baud);
    cfsetospeed(&t, (speed_t)baud);
    if (tcsetattr(fd, TCSANOW, &t) < 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* Bidirectional byte pump until the client closes or a fatal serial error. */
static void pump(int sfd, int ufd){
    enum { BUFSZ = 65536 };
    unsigned char *b = malloc(BUFSZ);
    if (!b) return;
    long tx = 0, rx = 0;
    int uerr = 0;
    for(;;){
        fd_set rf; FD_ZERO(&rf);
        FD_SET(sfd, &rf); FD_SET(ufd, &rf);
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        int r = select(sfd > ufd ? sfd+1 : ufd+1, &rf, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;   /* idle; Klipper polls keep-alive-ish */
        if (FD_ISSET(sfd, &rf)) {
            ssize_t n = recv(sfd, b, BUFSZ, 0);
            if (n <= 0) break;                 /* client closed / error */
            /* write all to serial (blocking-ish via nonblock + loop) */
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(ufd, b + off, (size_t)(n - off));
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(200); continue; }
                    uerr++; break;
                }
                off += w;
            }
            tx += (long)n;
        }
        if (FD_ISSET(ufd, &rf)) {
            ssize_t n = read(ufd, b, BUFSZ);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (errno == EIO || errno == ENODEV || errno == ENXIO) { uerr++; break; }
                continue;
            }
            if (n == 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = send(sfd, b + off, (size_t)(n - off), MSG_NOSIGNAL);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(200); continue; }
                    break;
                }
                off += w;
            }
            if (off < n) break;                /* client gone */
            rx += (long)n;
        }
        if (uerr > 4) { fprintf(stderr, "serial errors: dropping client\n"); break; }
    }
    free(b);
    fprintf(stderr, "pump done (host->mcu %ld B, mcu->host %ld B)\n", tx, rx);
    fflush(stderr);
}

int main(int argc, char **argv){
    if (argc > 1) g_port = atoi(argv[1]);
    if (argc > 2) { strncpy(g_uart, argv[2], sizeof g_uart - 1); }
    if (argc > 3) g_baud = atoi(argv[3]);
    g_verbose = 1;   /* we are nohup'd; always log */

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)g_port);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "bind :%d: %s\n", g_port, strerror(errno));
        return 1;
    }
    if (listen(srv, 1) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "ad4-bridge: TCP :%d <-> %s @%d (static, no-python)\n",
            g_port, g_uart, g_baud);
    fflush(stderr);

    for(;;){
        struct sockaddr_in ca; socklen_t cl = sizeof ca;
        int cli = accept(srv, (struct sockaddr *)&ca, &cl);
        if (cli < 0) { if (errno == EINTR) continue; usleep(100000); continue; }
        char ip[64];
        fprintf(stderr, "client %s:%d connected\n",
                inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof ip) ? ip : "?",
                ntohs(ca.sin_port));
        int one = 1;
        setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int ufd = uart_open(g_baud);
        if (ufd < 0) { close(cli); usleep(200000); continue; }
        pump(cli, ufd);
        close(ufd); close(cli);
        fprintf(stderr, "client gone; UART released, waiting for reconnect\n");
        fflush(stderr);
    }
    return 0;
}
