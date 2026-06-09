/* ===========================================================================
 * twrp_fix.c  —  LD_PRELOAD shim for the TWRP `recovery_real` binary
 * ===========================================================================
 *
 * Role in the decrypt stack
 * -------------------------
 * This library is preloaded into TWRP itself (pushed to /system/bin/
 * throw_logger.so and loaded by the recovery process). Its job is to keep the
 * `twrp decrypt` code path alive long enough to reach dm-default-key, and to
 * trace where it dies.
 *
 * On this device the FBE decrypt path forks a worker process and spins up
 * several auxiliary threads (notably a HWComposer display thread). When one of
 * those threads hits an uncaught C++ exception or calls abort(), libc would
 * normally tear down the WHOLE process via exit_group(), killing decrypt before
 * it finishes. The key trick here is to convert every fatal path into a
 * THREAD-only exit (SYS_exit / 93) instead of a process exit (SYS_exit_group /
 * 94), so the offending thread dies but decrypt keeps going.
 *
 * What it hooks
 * -------------
 *   std::terminate / abort / raise(SIGABRT) / kill(SIGABRT)
 *                         -> log + exit the current thread only (SYS_exit 93)
 *   ioctl()              -> fake-success for F2FS ioctls (type 0xf5) that fail
 *                           in recovery; trace device-mapper ioctls (type 0xfd)
 *                           so we can see whether decrypt reaches dm-default-key
 *   AServiceManager_waitForService / _checkService -> trace keystore2 lookups
 *   open()               -> trace successfully opened paths
 *
 * Everything is written with raw AArch64 syscalls and no libc startup, so the
 * shim can be compiled `-nostdlib` and injected very early.
 *
 * Build:  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 \
 *             -o twrp_fix.so twrp_fix.c
 * (the "noreturn function returns" warnings are expected and harmless)
 * ===========================================================================
 */
#define bool int
#include <stddef.h>
typedef int pid_t;

/* Write a NUL-terminated string straight to stderr via SYS_write (x8=64). */
static void wr(const char* s){
    if(!s||!*s)return;
    const char* e=s;while(*e)e++;
    __asm__ volatile(
        "mov x8,#64\nmov x0,#2\nmov x1,%0\nsub x2,%1,%0\nsvc #0\n"
        ::"r"(s),"r"(e):"x0","x1","x2","x8","memory");}

/* --- tiny number formatters (must be defined before first use) --- */
static void wrhex(unsigned long v){
    char h[17];int i=15;h[16]=0;
    do{h[i--]="0123456789abcdef"[v&0xf];v>>=4;}while(i>=0);
    wr("0x");wr(h);}

static void wrhex2(unsigned char v){
    char h[3];h[2]=0;
    h[0]="0123456789abcdef"[v>>4];
    h[1]="0123456789abcdef"[v&0xf];
    wr(h);}

static void wrint(long v){
    char buf[22];int i=21;buf[i]='\0';
    long a=(v<0)?-v:v;
    if(!a){buf[--i]='0';}else{while(a){buf[--i]=(char)('0'+(a%10));a/=10;}}
    if(v<0)buf[--i]='-';
    wr(buf+i);wr("\n");}

extern void* dlsym(void*,const char*) __attribute__((weak));

__attribute__((constructor)) static void init(void){
    wr("=== twrp_fix v3.2 loaded ===\n");}

/* Exit the CURRENT THREAD only (SYS_exit 93), never the whole process. This is
 * the core of the fix: a crashing display/worker thread must not take decrypt
 * down with it. */
static void __attribute__((noreturn)) do_exit(void){
    wr("[twrp_fix] -> exit_thread\n");
    register long x8 __asm__("x8")=93; /* exit thread only */
    register long x0 __asm__("x0")=0;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory");
    __builtin_unreachable();}

/* ioctl: fake-success F2FS ioctls that fail under recovery, and trace DM ones.
 *   type byte (req>>8 & 0xff): 0xf5 = F2FS, 0xfd = device-mapper. */
int ioctl(int fd,unsigned long req,long arg){
    typedef int(*fn)(int,unsigned long,long);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"ioctl");
    if(!real)return -1;
    unsigned char t=(req>>8)&0xff,n=req&0xff;
    if(t==0xfd){wr("[DM] nr=0x");wrhex2(n);wr(" fd=");wrint(fd);}
    int r=real(fd,req,arg);
    if(r<0&&t==0xf5){wr("[twrp_fix] F2FS->OK\n");return 0;}
    if(r<0&&t==0xfd){wr("[DM FAIL] nr=0x");wrhex2(n);wr("\n");}
    else if(r==0&&t==0xfd){wr("[DM OK] nr=0x");wrhex2(n);wr("\n");}
    return r;}

/* std::terminate — log the return address (so we can identify which library
 * threw) and kill only this thread. */
void _ZSt9terminatev(void){
    register unsigned long lr __asm__("x30");
    wr("[twrp_fix] std::terminate RA=");wrhex(lr);wr("\n");
    do_exit();}

__attribute__((noreturn)) void abort(void){
    wr("[twrp_fix] abort()\n");do_exit();}

int raise(int sig){
    if(sig==6){wr("[twrp_fix] raise(SIGABRT)\n");do_exit();}
    typedef int(*fn)(int);static fn r=0;
    if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"raise");
    return r?r(sig):-1;}

int kill(pid_t pid,int sig){
    if(sig==6){wr("[twrp_fix] kill(SIGABRT)\n");do_exit();}
    typedef int(*fn)(pid_t,int);static fn r=0;
    if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"kill");
    return r?r(pid,sig):-1;}

/* Trace TWRP's keystore2 service lookups so we can see FOUND vs NULL. */
void* AServiceManager_waitForService(const char* n){
    wr("[TWRP WFS] ");if(n)wr(n);wr("\n");
    typedef void*(*fn)(const char*);static fn r=0;
    if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"AServiceManager_waitForService");
    void* res=r?r(n):0;
    wr(res?"[TWRP WFS] FOUND\n":"[TWRP WFS] NULL\n");return res;}

void* AServiceManager_checkService(const char* n){
    wr("[TWRP CHK] ");if(n)wr(n);wr("\n");
    typedef void*(*fn)(const char*);static fn r=0;
    if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"AServiceManager_checkService");
    void* res=r?r(n):0;
    wr(res?"[TWRP CHK] FOUND\n":"[TWRP CHK] NULL\n");return res;}

/* Trace every file TWRP manages to open (useful to confirm key material and
 * block devices are reachable). O_CREAT (0100) carries a mode arg on the stack. */
int open(const char* p,int f,...){
    typedef int(*fn)(const char*,int,...);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"open");
    int fd=-1;
    if(f&0100){void* m;__asm__ volatile("ldr %0,[sp]":"=r"(m));fd=r?r(p,f,m):-1;}
    else fd=r?r(p,f):-1;
    if(p&&fd>=0){wr("[O] ");wr(p);wr("\n");}
    return fd;}
