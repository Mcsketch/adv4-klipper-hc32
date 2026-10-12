/*
 * frame_xray — ground-truth the HC32 ROM's reply to a single data frame.
 *
 * Does the exact vendor sequence (BOOT0=1 + NRST pulse -> sync 0x43), then:
 *   1. drains and dumps ANY bytes the ROM sent with/after the sync reply
 *      (reveals if the ROM sends more than the single 0x07 sync byte);
 *   2. sends ONE 1030-byte data frame built exactly like iap_xmit does
 *      (frame 0 of the given hex);
 *   3. waits 3s and dumps every byte the ROM replies, with inter-byte timing.
 *
 * Usage: frame_xray <klipper.hex>
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
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#define NRST_GPIO  34
#define BOOT0_GPIO 35
#define NRST_US    15680
#define GPIO_ROOT  "/sys/class/gpio"
#define PORT       "/dev/ttyS1"

static int g_exported[64] = {0};
static void die(const char*s){perror(s);exit(1);}
static void wr(const char*p,const char*v){int fd=open(p,O_WRONLY);if(fd<0)die(p);write(fd,v,strlen(v));close(fd);}
static void gpio_export(int n){
    char p[128],dp[128],b[16];
    snprintf(p,sizeof p,GPIO_ROOT"/export");
    snprintf(dp,sizeof dp,GPIO_ROOT"/gpio%d/direction",n);
    snprintf(b,sizeof b,"%d",n);
    if(access(dp,F_OK)!=0){int fd=open(p,O_WRONLY);if(fd<0)die("export");write(fd,b,strlen(b));close(fd);g_exported[n]=1;}
    int fd=open(dp,O_WRONLY); if(fd>=0){write(fd,"out",3);close(fd);}
    usleep(20000);
}
static void gpio_set(int n,int v){char p[128];snprintf(p,sizeof p,GPIO_ROOT"/gpio%d/value",n);wr(p,v?"1":"0");}
static void nrst_pulse(void){gpio_set(NRST_GPIO,0);usleep(NRST_US);gpio_set(NRST_GPIO,1);usleep(NRST_US);}
static long long now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000LL+t.tv_nsec/1000000LL;}

static int ser_open(int baud){
    int fd=open(PORT,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){fprintf(stderr,"open %s: %s\n",PORT,strerror(errno));return -1;}
    struct termios t; if(tcgetattr(fd,&t)){close(fd);return -1;}
    cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD;
    t.c_cflag&=~CSTOPB;t.c_cflag&=~PARENB;
    t.c_cflag&=~CSIZE;t.c_cflag|=CS8;
    cfsetispeed(&t,(speed_t)baud);cfsetospeed(&t,(speed_t)baud);
    tcsetattr(fd,TCSANOW,&t);tcflush(fd,TCIOFLUSH);
    return fd;
}
/* read one byte with a timeout; returns 1 if got a byte */
static int rd1(int fd,unsigned char*b,int ms){
    while(ms-->=0){int r=read(fd,b,1);if(r==1)return 1;if(r<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)return 0;usleep(1000);}
    return 0;
}
static void ser_wr(int fd,const unsigned char*b,int n){int o=0;while(o<n){int w=write(fd,b+o,n-o);if(w<0)die("write");o+=w;}}

static uint16_t crc16_iap(const unsigned char*p,int n){
    uint16_t c=0;
    for(int i=0;i<n;i++){c^=(uint16_t)p[i]<<8;for(int b=0;b<8;b++)c=(c&0x8000)?(uint16_t)((c<<1)^0x1021):(uint16_t)(c<<1);}
    return c;
}

/* minimal iHEX: load into a flat buffer at min address */
static int hx(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return 0;}
static int hexn(const char*s,int n){int v=0;for(int i=0;i<n;i++)v=(v<<4)|hx(s[i]);return v;}
static int ihex_load(const char*path,uint8_t**out,uint32_t*len,uint32_t*base){
    FILE*f=fopen(path,"rb"); if(!f){fprintf(stderr,"open %s: %s\n",path,strerror(errno));return 1;}
    uint32_t cap=1u<<20; uint8_t*buf=malloc(cap); uint32_t b=0,maxa=0; int have=0; uint32_t minabs=~0u;
    char line[1024];
    while(fgets(line,sizeof line,f)){
        line[strcspn(line,"\r\n")]=0;
        if(line[0]!=':'||(int)strlen(line)<11)continue;
        int cnt=hexn(line+1,2);uint16_t off=hexn(line+3,4);int typ=hexn(line+7,2);
        if(typ==0x04||typ==0x05){b=((uint32_t)hexn(line+9,4))<<16;continue;}
        if(typ==0x01)break;
        if(typ!=0x00||(int)strlen(line)<5+cnt*2+5)continue;
        for(int i=0;i<cnt;i++){
            int byte=hexn(line+9+2*i,2);uint32_t abs=b+(uint32_t)off+i;
            if(!have||abs<minabs)minabs=abs;
            if(abs>=cap){while(abs>=cap)cap<<=1;buf=realloc(buf,cap);}
            buf[abs-minabs]=(uint8_t)byte;
            if(abs>maxa)maxa=abs;have=1;
        }
    }
    fclose(f);
    if(!have){free(buf);return 1;}
    *out=buf;*len=maxa-minabs+1;*base=minabs;return 0;
}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: frame_xray <hex> [nframes] [baud]\n");return 2;}
    if(geteuid()!=0){fprintf(stderr,"run as root\n");return 2;}
    uint8_t*img;uint32_t len,base;
    if(ihex_load(argv[1],&img,&len,&base)){fprintf(stderr,"ihex fail\n");return 1;}
    int baud=(argc>3)?atoi(argv[3]):115200;
    printf("loaded %s: %u bytes @ 0x%08x  (baud %d)\n",argv[1],len,base,baud);

    gpio_export(BOOT0_GPIO);gpio_export(NRST_GPIO);
    int fd=ser_open(baud);
    gpio_set(BOOT0_GPIO,1);sleep(1);nrst_pulse();

    /* sync */
    unsigned char c=0x43;int synced=0;unsigned char rev=0;
    for(int t=0;t<60&&!synced;t++){
        tcflush(fd,TCIOFLUSH);ser_wr(fd,&c,1);
        if(rd1(fd,&rev,100)==1&&(rev==0x06||rev==0x07||rev==0x79))synced=1;
        if(t%5==0)nrst_pulse();
    }
    if(!synced){fprintf(stderr,"no sync\n");return 1;}
    printf("SYNC ok (reply 0x%02x)\n",rev);

    /* STEP 1: drain any extra bytes the ROM sent with the sync reply */
    printf("\n--- STEP 1: drain pending RX after sync (200ms window) ---\n");
    long long t0=now_ms();int got=0;
    while(now_ms()-t0<200){
        unsigned char b;
        if(rd1(fd,&b,10)==1){printf("  +%4lldms  0x%02X\n",now_ms()-(t0+10*(got+1)),b);got++;}
        else break;
    }
    if(!got)printf("  (no extra bytes — sync reply was a single 0x%02x)\n",rev);
    else printf("  (%d extra byte(s) after sync!)\n",got);

    /* STEP 2: build + send ONE data frame (frame 0), exactly like iap_xmit */
    unsigned char pkt[1030];
    memset(pkt,0,sizeof pkt);
    pkt[0]=0x01;
    pkt[1]=(unsigned char)((base+0)&0xff);
    int n0=len<1024?(int)len:1024;
    pkt[2]=(unsigned char)((n0>>8)&0xff);
    pkt[3]=(unsigned char)(n0&0xff);
    memcpy(pkt+4,img,n0);
    uint16_t crc=crc16_iap(pkt+4,1024);
    pkt[1028]=(unsigned char)((crc>>8)&0xff);
    pkt[1029]=(unsigned char)(crc&0xff);
    printf("\n--- STEP 2: send frame 0 (1030 bytes) ---\n");
    printf("  hdr[0..3]=%02X %02X %02X %02X  crc=%04X  (first 16 data: ",pkt[0],pkt[1],pkt[2],pkt[3],crc);
    for(int i=0;i<16;i++)printf("%02X ",pkt[4+i]);
    printf(")\n");
    long long ts=now_ms();
    ser_wr(fd,pkt,1030);
    printf("  sent at +%lldms\n",now_ms()-ts);

    /* STEP 3: send N frames in sequence, accept BOTH 0x06 and 0x07 as ack,
     * capture the reply to each. Tells us if 0x07 is the valid HD frame-ack
     * (protocol progresses through the image) or a reject (ROM stops replying). */
    int NFRAMES = (argc>2)?atoi(argv[2]):5;
    printf("\n--- STEP 3: send %d frames, accept 0x06/0x07 as ack, per-frame reply ---\n",NFRAMES);
    int fidx=0, baseaddr=base;
    for(int f=0; f<NFRAMES; f++){
        uint32_t off=f*1024;
        int n=(len-off)<1024?(int)(len-off):1024;
        unsigned char pkt[1030];
        memset(pkt,0,sizeof pkt);
        pkt[0]=0x01;
        pkt[1]=(unsigned char)((baseaddr+off)&0xff);
        pkt[2]=(unsigned char)((n>>8)&0xff);
        pkt[3]=(unsigned char)(n&0xff);
        memcpy(pkt+4,img+off,n);
        uint16_t crc=crc16_iap(pkt+4,1024);
        pkt[1028]=(unsigned char)((crc>>8)&0xff);
        pkt[1029]=(unsigned char)(crc&0xff);
        long long ts=now_ms();
        ser_wr(fd,pkt,1030);
        /* wait up to 1s for a reply byte */
        unsigned char r=0; int got=rd1(fd,&r,1000);
        printf("  frame %d @0x%08x (%d B): %s%s\n", f, baseaddr+off, n,
            got? "reply 0x":"TIMEOUT",
            got? (r==0x06?" (0x06 OK)":" (0x07?!)"):
            (r==0x07?"":" (no ack)"));
        /* drain any extra reply bytes this frame produced */
        long long d0=now_ms();
        while(now_ms()-d0<150){ unsigned char x; if(rd1(fd,&x,10)==1)printf("      +extra 0x%02X\n",x); else break; }
        if(!got){ printf("  *** ROM stopped replying at frame %d ***\n",f); break; }
        fidx++;
        usleep(2000);
    }

    /* release to normal boot */
    gpio_set(BOOT0_GPIO,0);nrst_pulse();
    close(fd);
    return 0;
}
