/* ===========================================================================
 * hwsm_log.c  —  LD_PRELOAD shim for `hwservicemanager`
 * ===========================================================================
 *
 * Role in the decrypt stack
 * -------------------------
 * The HIDL half of the binder world. keymint pulls in some HIDL dependencies,
 * so a hwservicemanager has to be context-manager on /dev/hwbinder as well.
 * This is essentially the HIDL counterpart of sm_stab3.c:
 *
 *   - ioctl(BINDER_SET_CONTEXT_MGR) -> retry up to 60x100ms on /dev/hwbinder.
 *   - abort() -> spin (wfi) instead of dying.
 *   - VintfObject::fetchDeviceHalManifest -> trace (no force here; hwSM tends
 *       to load its manifest fine, we only log the result).
 *   - open/openat -> trace interesting paths (incl. /dev/hw*) with errno so we
 *       can spot HIDL manifest / hwbinder lookup failures.
 *
 * Build:  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 \
 *             -o hwsm_log.so hwsm_log.c
 * ===========================================================================
 */
#define bool int
#include <stddef.h>

/* Write a NUL-terminated string straight to stderr via SYS_write (x8=64). */
static void wr(const char* s){
    if(!s||!*s)return;
    const char* e=s;while(*e)e++;
    __asm__ volatile(
        "mov x8,#64\nmov x0,#2\nmov x1,%0\nsub x2,%1,%0\nsvc #0\n"
        ::"r"(s),"r"(e):"x0","x1","x2","x8","memory");}

static void wrint(long v){
    char buf[22];int i=21;buf[i]='\0';
    long a=(v<0)?-v:v;
    if(!a){buf[--i]='0';}
    else{while(a){buf[--i]=(char)('0'+(a%10));a/=10;}}
    if(v<0)buf[--i]='-';
    wr(buf+i);wr("\n");}

extern void* dlsym(void*,const char*) __attribute__((weak));

__attribute__((constructor)) static void init(void){wr("=== hwsm_log loaded ===\n");}

/* ─── hwbinder context manager retry ─── */
static void sleep_100ms(void){
    long ts[2]={0,100000000L};
    register long x8 __asm__("x8")=101;
    register long x0 __asm__("x0")=(long)ts;
    register long x1 __asm__("x1")=0;
    __asm__ volatile("svc #0":"+r"(x0),"+r"(x1):"r"(x8):"memory");}

int ioctl(int fd,unsigned long req,long arg){
    typedef int(*fn)(int,unsigned long,long);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"ioctl");
    if(!real)return -1;
    int r=real(fd,req,arg);
    /* hwbinder BINDER_SET_CONTEXT_MGR (same codes, but fd from /dev/hwbinder) */
    if((req==0x40046207ul||req==0x4018620dul)&&r!=0){
        wr("[hwSM] CM failed, retrying...\n");
        int i=0;
        while(r!=0&&i<60){sleep_100ms();r=real(fd,req,arg);i++;}
        if(r==0)wr("[hwSM] became CM!\n");
        else wr("[hwSM] CM GAVE UP\n");}
    return r;}

/* ─── Catch abort ─── */
__attribute__((noreturn)) void abort(void){
    wr("[hwSM] ABORT INTERCEPTED - spinning\n");
    while(1){__asm__ volatile("wfi":::"memory");}
    __builtin_unreachable();}

/* ─── hwservicemanager liblog mirror ─── */
int __android_log_buf_write(int b,int p,const char* t,const char* m){
    wr("[hwSM LOG] ");if(t){wr(t);wr(":");}if(m)wr(m);wr("\n");return 1;}
int __android_log_print(int p,const char* t,const char* fmt,const char* arg1,...){
    wr("[hwSM LOG] ");if(t){wr(t);wr(":");}if(fmt)wr(fmt);
    if(arg1&&(unsigned long)arg1>4096UL){wr(" -> ");wr(arg1);}
    wr("\n");return 1;}

/* ─── VINTF trace (hwSM also checks VINTF for the HIDL manifest) ─── */
/* No specific HIDL symbol to stub, but if fetchDeviceHalManifest is exported
   from the same libvintf.so we can at least trace it. */
int _ZN7android5vintf11VintfObject22fetchDeviceHalManifestEPNS0_11HalManifestEPNSt3__112basic_stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEE(void*self,void*man,void*err){
    wr("[hwSM] fetchDeviceHalManifest enter\n");
    typedef int(*fdm_t)(void*,void*,void*);static fdm_t real=0;
    if(!real)real=(fdm_t)dlsym((void*)-1L,
        "_ZN7android5vintf11VintfObject22fetchDeviceHalManifestEPNS0_11HalManifestEPNSt3__112basic_stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEE");
    int r=real?real(self,man,err):-99;
    if(r==0){wr("[hwSM] fetchDeviceHalManifest OK\n");}
    else{wr("[hwSM] fetchDeviceHalManifest FAIL r=");wrint(r);}
    return r;}

/* ─── openat with full trace and precise errno ─── */
static int sw(const char* s,const char* p){
    if(!s||!p)return 0;
    int i=0;while(p[i]&&s[i]&&p[i]==s[i])i++;return !p[i];}

static int interesting(const char* p){
    if(!p)return 0;
    return sw(p,"/vendor")||sw(p,"/apex")||sw(p,"/odm")||
           sw(p,"/product")||sw(p,"/system_ext")||
           sw(p,"/system/etc/vintf")||sw(p,"/dev/hw");}

static long sys_openat(int d,const char* p,int f){
    register long x8 __asm__("x8")=56;
    register long x0 __asm__("x0")=(long)(unsigned)d;
    register long x1 __asm__("x1")=(long)p;
    register long x2 __asm__("x2")=(long)(f & ~0100);
    register long x3 __asm__("x3")=0;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
    return x0;}

static void sys_close(long fd){
    register long x8 __asm__("x8")=57;
    register long x0 __asm__("x0")=fd;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory");}

int openat(int d,const char* p,int f,...){
    typedef int(*fn)(int,const char*,int,...);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"openat");
    int fd=-1;
    if(f&0100){void* m;__asm__ volatile("ldr %0,[sp]":"=r"(m));fd=r?r(d,p,f,m):-1;}
    else fd=r?r(d,p,f):-1;
    if(interesting(p)){
        wr("[hwSM OAT] ");wr(p);
        if(fd<0){
            long e=sys_openat(d,p,f);
            if(e>=0)sys_close(e);
            wr(" FAIL errno=");wrint(e<0?e:-999L);
        }else{
            wr(" OK\n");
        }
    }
    return fd;}

int open(const char* p,int f,...){
    typedef int(*fn)(const char*,int,...);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"open");
    int fd=-1;
    if(f&0100){void* m;__asm__ volatile("ldr %0,[sp]":"=r"(m));fd=r?r(p,f,m):-1;}
    else fd=r?r(p,f):-1;
    if(interesting(p)){
        wr("[hwSM OPEN] ");wr(p);
        if(fd<0)wr(" FAIL\n");else wr(" OK\n");}
    return fd;}
