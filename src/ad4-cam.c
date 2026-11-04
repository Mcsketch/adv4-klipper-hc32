/*
 * ad4-cam - MJPEG camera server for the Adventurer 4's SoC, static ARM binary.
 *
 * The chamber camera is a UVC "USB 2.0 PC Camera" (058f:3841) on /dev/video0.
 * It compresses to MJPEG itself, up to 1280x1024 at 30 fps, with Huffman
 * tables in every frame, so this only passes frames through: no encoding,
 * almost no CPU.
 *
 *   GET /stream     multipart/x-mixed-replace MJPEG   (also /?action=stream)
 *   GET /snapshot   one JPEG                          (also /?action=snapshot)
 *
 * The URLs match mjpg-streamer's, which the stock UI served on the same port,
 * so Moonraker's "mjpegstreamer" webcam service works as is. The camera is
 * opened when the first client arrives and closed 10 s after the last one
 * leaves. Each client gets the newest frame whenever its socket can take one;
 * a slow client skips frames instead of holding up the others.
 *
 * Usage: ad4-cam [PORT] [WIDTHxHEIGHT] [FPS]     (defaults 8080 1280x720 15)
 *
 * Kernel 3.4's v4l2_buffer holds a 32-bit timeval: build with a 32-bit
 * time_t (the Makefile does), or VIDIOC_DQBUF has the wrong ioctl number.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/videodev2.h>

#define MAXC 4
#define NBUF 4
#define BOUNDARY "ad4camframe"

typedef struct {
    int fd, stream, done;             /* done: close once buf is sent */
    char req[512]; int rlen, parsed;
    char *buf; int len, off, cap;
    unsigned seq;                     /* last frame queued */
    long t0;
} client;

static client cl[MAXC];
static int cam = -1, skip;
static void *bm[NBUF]; static size_t bl[NBUF];
static unsigned char *frame; static int flen, fcap;
static unsigned fseq;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static void cam_close(void) {
    if (cam < 0) return;
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < NBUF; i++) if (bm[i] && bm[i] != MAP_FAILED) munmap(bm[i], bl[i]);
    memset(bm, 0, sizeof bm);
    close(cam);
    cam = -1;
    fprintf(stderr, "ad4-cam: camera closed\n");
}

static int cam_open(int w, int h, int fps) {
    if ((cam = open("/dev/video0", O_RDWR | O_NONBLOCK)) < 0) return -1;
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = w; f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    struct v4l2_streamparm p;
    memset(&p, 0, sizeof p);
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    p.parm.capture.timeperframe.numerator = 1;
    p.parm.capture.timeperframe.denominator = fps;
    struct v4l2_requestbuffers r;
    memset(&r, 0, sizeof r);
    r.count = NBUF; r.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; r.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam, VIDIOC_S_FMT, &f) || f.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
        ioctl(cam, VIDIOC_S_PARM, &p) || ioctl(cam, VIDIOC_REQBUFS, &r) || r.count > NBUF)
        goto fail;
    for (unsigned i = 0; i < r.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = r.type; b.memory = r.memory; b.index = i;
        if (ioctl(cam, VIDIOC_QUERYBUF, &b)) goto fail;
        bl[i] = b.length;
        bm[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, cam, b.m.offset);
        if (bm[i] == MAP_FAILED || ioctl(cam, VIDIOC_QBUF, &b)) goto fail;
    }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam, VIDIOC_STREAMON, &t)) goto fail;
    skip = 5;                         /* let auto exposure settle */
    fprintf(stderr, "ad4-cam: camera open %ux%u MJPEG %d fps\n",
            f.fmt.pix.width, f.fmt.pix.height, fps);
    return 0;
fail:
    fprintf(stderr, "ad4-cam: camera setup failed: %s\n", strerror(errno));
    cam_close();
    return -1;
}

/* Returns -1 when the camera is gone. */
static int cam_read(void) {
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam, VIDIOC_DQBUF, &b)) return errno == EAGAIN ? 0 : -1;
    int n = (int)b.bytesused;
    if (skip) skip--;
    else if (n > 2) {
        if (n > fcap) { fcap = n + 65536; frame = realloc(frame, fcap); }
        memcpy(frame, bm[b.index], n);
        flen = n;
        fseq++;
    }
    return ioctl(cam, VIDIOC_QBUF, &b) ? -1 : 0;
}

static void drop(client *c) {
    close(c->fd);
    free(c->buf);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void queue(client *c, const char *hdr, const unsigned char *d, int n, const char *tail) {
    int hl = (int)strlen(hdr), tl = (int)strlen(tail);
    if (hl + n + tl > c->cap) { c->cap = hl + n + tl + 65536; c->buf = realloc(c->buf, c->cap); }
    memcpy(c->buf, hdr, hl);
    if (n) memcpy(c->buf + hl, d, n);
    memcpy(c->buf + hl + n, tail, tl);
    c->len = hl + n + tl;
    c->off = 0;
}

/* A complete request line: decide what the client gets. */
static void route(client *c) {
    char path[256] = "";
    c->parsed = 1;
    sscanf(c->req, "GET %255s", path);
    if (strstr(path, "stream")) {
        c->stream = 1;
        queue(c, "HTTP/1.0 200 OK\r\nCache-Control: no-cache, no-store\r\nPragma: no-cache\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n\r\n", NULL, 0, "");
    } else if (strstr(path, "snapshot")) {
        c->stream = 0;
    } else {
        queue(c, "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\n"
                 "ad4-cam: /stream or /snapshot\n", NULL, 0, "");
        c->done = 1;
    }
}

static void feed(client *c) {
    char h[256];
    if (!c->parsed || c->done || c->off < c->len || c->seq == fseq || !flen) return;
    c->seq = fseq;
    if (c->stream) {
        snprintf(h, sizeof h, "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", flen);
        queue(c, h, frame, flen, "\r\n");
    } else {
        snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nCache-Control: no-cache, no-store\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", flen);
        queue(c, h, frame, flen, "");
        c->done = 1;
    }
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080, w = 1280, h = 720;
    int fps = argc > 3 ? atoi(argv[3]) : 15;
    if (argc > 2) sscanf(argv[2], "%dx%d", &w, &h);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAXC; i++) cl[i].fd = -1;

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) {
        perror("ad4-cam: bind");
        return 1;
    }
    fcntl(ls, F_SETFL, O_NONBLOCK);
    fprintf(stderr, "ad4-cam: listening on :%d, %dx%d %d fps\n", port, w, h, fps);

    long idle = 0, tried = -10000;
    for (;;) {
        struct pollfd p[MAXC + 2];
        int np = 0, want = 0;
        long t = now_ms();
        for (int i = 0; i < MAXC; i++) {
            client *c = &cl[i];
            if (c->fd < 0) continue;
            if (c->parsed && !c->done) want = 1;
            /* a snapshot nobody could serve in 5 s */
            if (c->parsed && !c->stream && !c->done && t - c->t0 > 5000) {
                queue(c, "HTTP/1.0 503 Service Unavailable\r\n\r\n", NULL, 0, "");
                c->done = 1;
            }
        }
        if (want) idle = t;
        if (want && cam < 0 && t - tried > 2000) { tried = t; cam_open(w, h, fps); }
        if (cam >= 0 && t - idle > 10000) cam_close();

        p[np++] = (struct pollfd){ .fd = ls, .events = POLLIN };
        if (cam >= 0) p[np++] = (struct pollfd){ .fd = cam, .events = POLLIN };
        for (int i = 0; i < MAXC; i++)
            if (cl[i].fd >= 0)
                p[np++] = (struct pollfd){ .fd = cl[i].fd,
                    .events = POLLIN | (cl[i].off < cl[i].len ? POLLOUT : 0) };
        if (poll(p, np, 1000) < 0 && errno != EINTR) return 1;

        if (p[0].revents & POLLIN) {
            int fd = accept(ls, NULL, NULL);
            int i = 0;
            while (i < MAXC && cl[i].fd >= 0) i++;
            if (fd >= 0 && i == MAXC) {
                send(fd, "HTTP/1.0 503 Service Unavailable\r\n\r\n", 36, MSG_NOSIGNAL);
                close(fd);
            } else if (fd >= 0) {
                fcntl(fd, F_SETFL, O_NONBLOCK);
                memset(&cl[i], 0, sizeof cl[i]);
                cl[i].fd = fd;
                cl[i].seq = fseq;     /* nothing older than this request */
                cl[i].t0 = now_ms();
            }
        }
        if (cam >= 0 && (p[1].revents & (POLLIN | POLLERR)) && cam_read() < 0) {
            fprintf(stderr, "ad4-cam: camera lost: %s\n", strerror(errno));
            cam_close();
            for (int i = 0; i < MAXC; i++) if (cl[i].fd >= 0 && cl[i].parsed && !cl[i].done) drop(&cl[i]);
        }
        for (int i = 0; i < MAXC; i++) {
            client *c = &cl[i];
            if (c->fd < 0) continue;
            int j = 1 + (cam >= 0);
            while (j < np && p[j].fd != c->fd) j++;
            short re = j < np ? p[j].revents : 0;
            if (re & POLLIN) {
                char tmp[512];
                ssize_t r = read(c->fd, tmp, sizeof tmp);
                if (r <= 0) { if (r == 0 || errno != EAGAIN) { drop(c); continue; } }
                else if (!c->parsed) {
                    int k = r < (ssize_t)sizeof c->req - 1 - c->rlen ? (int)r : (int)sizeof c->req - 1 - c->rlen;
                    memcpy(c->req + c->rlen, tmp, k);
                    c->rlen += k;
                    c->req[c->rlen] = 0;
                    if (strstr(c->req, "\r\n\r\n") || strstr(c->req, "\n\n") ||
                        c->rlen == (int)sizeof c->req - 1)
                        route(c);
                }
            } else if (re & (POLLERR | POLLHUP)) { drop(c); continue; }
            if (c->off < c->len && (re & POLLOUT)) {
                ssize_t r = send(c->fd, c->buf + c->off, c->len - c->off, MSG_NOSIGNAL);
                if (r < 0 && errno != EAGAIN) { drop(c); continue; }
                if (r > 0) c->off += (int)r;
            }
            if (c->off == c->len && c->done) { drop(c); continue; }
            feed(c);
        }
    }
}
