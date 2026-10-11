#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
static double now_s(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec*1e-9;}
static double t0=0;
#define NRST_GPIO 34
#define BOOT0_GPIO 35
static void wrf(const char*p,const char*v){int fd=open(p,2);if(fd>=0){write(fd,v,strlen(v));close(fd);}}
static void gexp(int n){char p[64],b[16];snprintf(p,64,"/sys/class/gpio/gpio%d/direction",n);
  if(access(p,0)){snprintf(p,64,"/sys/class/gpio/export");snprintf(b,16,"%d",n);wrf(p,b);}
  snprintf(p,64,"/sys/class/gpio/gpio%d/direction",n);wrf(p,"out");usleep(20000);}
static void gset(int n,int v){char p[64];snprintf(p,64,"/sys/class/gpio/gpio%d/value",n);wrf(p,v?"1":"0");}
int main(){
  int fd=open("/dev/ttyS1",O_RDWR|O_NOCTTY|O_NONBLOCK);
  struct termios t;tcgetattr(fd,&t);cfmakeraw(&t);t.c_cflag|=CLOCAL|CREAD;
  t.c_cflag&=~CSTOPB;t.c_cflag&=~PARENB;t.c_cflag&=~CSIZE;t.c_cflag|=CS8;
  cfsetispeed(&t,B115200);cfsetospeed(&t,B115200);tcsetattr(fd,TCSANOW,&t);tcflush(fd,TCIOFLUSH);
  t0=now_s();
  gexp(BOOT0_GPIO);gexp(NRST_GPIO);gset(BOOT0_GPIO,1);sleep(1);
  gset(NRST_GPIO,0);usleep(15680);gset(NRST_GPIO,1);usleep(15680);
  usleep(100000); tcflush(fd,TCIOFLUSH);
  unsigned char c=0x43; write(fd,&c,1);
  fprintf(stderr,"[T%.3f] SENT 0x43, waiting...\n",now_s()-t0);
  for(int i=0;i<40;i++){
    unsigned char b; ssize_t r=read(fd,&b,1);
    if(r==1){fprintf(stderr,"[T%.3f] RECV 0x%02X\n",now_s()-t0,b);}
    usleep(25000);
  }
  // now try the STM32 get-pid (0x02) to see what ack the ROM gives
  fprintf(stderr,"[T%.3f] --- sending GET-PID 0x02 ---\n",now_s()-t0);
  tcflush(fd,TCIOFLUSH);
  unsigned char g=0x02; write(fd,&g,1);
  for(int i=0;i<20;i++){unsigned char b;ssize_t r=read(fd,&b,1);if(r==1)fprintf(stderr,"[T%.3f] RECV 0x%02X\n",now_s()-t0,b);usleep(20000);}
  // try ERASE 0x44 then 4 addr bytes 0x08000000
  fprintf(stderr,"[T%.3f] --- ERASE 0x44 + addr 0x08000000 ---\n",now_s()-t0);
  tcflush(fd,TCIOFLUSH);
  unsigned char e[5]={0x44,0x00,0x00,0x00,0x08}; write(fd,e,1);
  for(int i=0;i<10;i++){unsigned char b;ssize_t r=read(fd,&b,1);if(r==1)fprintf(stderr,"[T%.3f] RECV 0x%02X\n",now_s()-t0,b);usleep(20000);}
  write(fd,e+1,4);
  for(int i=0;i<20;i++){unsigned char b;ssize_t r=read(fd,&b,1);if(r==1)fprintf(stderr,"[T%.3f] RECV 0x%02X\n",now_s()-t0,b);usleep(20000);}
  gset(BOOT0_GPIO,0);gset(NRST_GPIO,0);usleep(15680);gset(NRST_GPIO,1);
  fprintf(stderr,"[done]\n");
  return 0;
}
