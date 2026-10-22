/* ad4_identify.c — full Klipper identify handshake against the MCU on /dev/ttyS1.
 * Protocol-correct: seq increments per accepted message, CRC16 validated on
 * every frame, retransmit (same seq) on NAK/timeout, 40-byte chunks until the
 * MCU clamps count to 0. Emits the raw (zlib) identify blob on stdout; logs on
 * stderr. Ground truth that BOTH directions of USART2/PA9-PA10 work.
 * Usage: ad4_identify <baud> <port>
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>

#define MSGID_IDENTIFY_REQ 0x01
#define MSGID_IDENTIFY_RESP 0x00
#define MSGID_STATS 0x4c
#define MSGID_CLEAR_SHUTDOWN 0x02 /* from out/klipper.dict; HF_IN_SHUTDOWN */
#define CHUNK 40
#define MAXBLOB 8192

static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000LL + ts.tv_nsec/1000000LL;
}
static uint16_t crc16_ccitt(const uint8_t*b,int n){
    uint16_t crc=0xffff;
    for(int i=0;i<n;i++){
        uint8_t data=b[i];
        data^=crc&0xff;
        data^=(uint8_t)((data&0x0f)<<4);
        crc=(uint16_t)(((uint16_t)data<<8)|(crc>>8));
        crc^=(uint16_t)(data>>4);
        crc^=(uint16_t)((uint16_t)data<<3);
    }
    return crc;
}
static int vlq32(uint8_t*out,uint32_t v){
    int n=0;
    if(v>=0x0c000000u) out[n++]=(uint8_t)(((v>>28)&0x7f)|0x80);
    if(v>=0x00180000u) out[n++]=(uint8_t)(((v>>21)&0x7f)|0x80);
    if(v>=0x00003000u) out[n++]=(uint8_t)(((v>>14)&0x7f)|0x80);
    if(v>=0x00000060u) out[n++]=(uint8_t)(((v>>7)&0x7f)|0x80);
    out[n++]=(uint8_t)(v&0x7f); /* matches MCU encode_int: final byte masked */
    return n;
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

/* stream parse state */
static uint8_t sbuf[4096]; static int slen=0;
static uint8_t blob[MAXBLOB]; static int bloblen=0;
static int got_resp=0; static uint32_t resp_off=0;

/* try to extract one frame from sbuf; returns 1 if consumed (any kind) */
static int pump_frame(void){
    if(slen<6) return 0;
    int ln=sbuf[0];
    if(ln<5||ln>128){ memmove(sbuf,sbuf+1,slen-1); slen--; return 1; } /* resync */
    if(slen<ln) return 0;
    if(sbuf[ln-1]!=0x7e){ memmove(sbuf,sbuf+1,slen-1); slen--; return 1; }
    uint16_t crc=crc16_ccitt(sbuf,ln-3);
    uint16_t field=(uint16_t)((sbuf[ln-3]<<8)|sbuf[ln-2]);
    if(crc!=field){
        fprintf(stderr,"  bad CRC frame: ");
        for(int i=0;i<ln&&i<24;i++) fprintf(stderr,"%02x ",sbuf[i]);
        fprintf(stderr,"\n");
        memmove(sbuf,sbuf+1,slen-1); slen--; return 1;
    }
    if(getenv("AD4_RAW")){
        fprintf(stderr,"  frame[%d]: ",ln);
        for(int i=0;i<ln;i++) fprintf(stderr,"%02x ",sbuf[i]);
        fprintf(stderr,"\n");
    }
    int msgid = (ln==5) ? -1 : sbuf[2]; /* empty frame = ack/nak */
    if(msgid==MSGID_IDENTIFY_RESP){
        /* frame: [len][seq][00][vlq offset x n][count byte][data x count][crc][7e]
         * (%.*s in Klipper encodes a length byte then raw bytes) */
        uint32_t v=sbuf[3]&0x7f; int n=1;
        while(sbuf[2+n]&0x80){ n++; v=(v<<7)|(sbuf[2+n]&0x7f); }
        resp_off=v;
        int dstart=4+n;
        int dlen=ln-3-dstart;
        if(v!=(uint32_t)bloblen){
            fprintf(stderr,"  resp off=%u unexpected (bloblen=%d) — ignored\n",v,bloblen);
        } else {
            got_resp=1;
            if(dlen>0){
                if(bloblen+dlen<=MAXBLOB){
                    memcpy(blob+bloblen,&sbuf[dstart],dlen); bloblen+=dlen;
                } else fprintf(stderr,"  BLOB FULL\n");
            }
        }
    } else if(msgid==MSGID_STATS){
        fprintf(stderr,"  (stats frame)\n");
    } else if(msgid==-1){
        fprintf(stderr,"  (ack/nak empty frame)\n");
    } else {
        fprintf(stderr,"  frame msgid=0x%02x len=%d\n",msgid,ln);
    }
    memmove(sbuf,sbuf+ln,slen-ln); slen-=ln;
    return 1;
}

int main(int argc,char**argv){
    int baud = argc>1?atoi(argv[1]):115200;
    const char *port = argc>2?argv[2]:"/dev/ttyS1";
    int fd=open_raw(port,baud);
    if(fd<0) return 2;
    /* Preflight: a previous bad frame may have left the MCU in shutdown
     * (it re-sends 0x4b but still answers HF_IN_SHUTDOWN commands). Send
     * clear_shutdown once per seq 0..15; exactly one matches next_sequence. */
    for(int s=0; s<16; s++){
        uint8_t f[6]={6,0x10|(uint8_t)(s&0x0f),MSGID_CLEAR_SHUTDOWN,0,0,0x7e};
        uint16_t crc=crc16_ccitt(f,3);
        f[3]=(uint8_t)(crc>>8); f[4]=(uint8_t)(crc&0xff);
        write(fd,f,6);
        usleep(30000);
        int space=(int)sizeof sbuf - slen;
        int r=read(fd,sbuf+slen,space>0?space:0);
        if(r>0) slen+=r;
        while(pump_frame());
    }
    fprintf(stderr,"-- clear_shutdown preflight done, starting identify --\n");
    uint8_t seq=0;
    uint32_t offset=0;
    int done=0;
    while(!done){
        got_resp=0;
        /* MCU's next_sequence persists across our connections: cycle seq on
         * retry until one matches; once accepted, resume strict increment. */
        for(int retry=0; retry<16 && !got_resp; retry++){
            uint8_t tryseq=(uint8_t)((seq+retry)&0x0f);
            /* build identify request: [len][0x10|seq][01][vlq(off)][40][crc][7e] */
            uint8_t f[16]; int p=3;
            f[2]=MSGID_IDENTIFY_REQ;
            p+=vlq32(&f[p],offset);
            f[p++]=CHUNK;
            f[0]=(uint8_t)(p+3); /* bytes 0..p-1 = p, + crc(2) + sync(1) */
            f[1]=(uint8_t)(0x10|tryseq);
            uint16_t crc=crc16_ccitt(f,p);
            f[p++]=(uint8_t)(crc>>8); f[p++]=(uint8_t)(crc&0xff); f[p++]=0x7e;
            write(fd,f,p);
            if(retry==0||got_resp==0)
                fprintf(stderr,"-> identify off=%u seq=%d (try %d)\n",offset,tryseq,retry+1);
            long long t0=now_ms();
            while(now_ms()-t0<500 && !got_resp){
                int space=(int)sizeof sbuf - slen;
                int r=read(fd,sbuf+slen,space>0?space:0);
                if(r>0) slen+=r;
                else if(r<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK){ perror("read"); return 3; }
                while(pump_frame());
                if(!got_resp) usleep(2000);
            }
            if(got_resp) seq=(uint8_t)((tryseq+1)&0x0f);
        }
        if(!got_resp){ fprintf(stderr,"FAILED at offset %u after 16 seq tries\n",offset); return 4; }
        /* MCU clamps count to 0 when offset>=isize: empty data => done */
        if(bloblen==(int)offset) done=1;
        else offset=bloblen;
    }
    fwrite(blob,1,bloblen,stdout);
    fprintf(stderr,"identify blob: %d bytes\n",bloblen);
    return 0;
}
