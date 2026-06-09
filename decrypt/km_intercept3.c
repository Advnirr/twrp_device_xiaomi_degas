/* ===========================================================================
 * km_intercept3.c  —  LD_PRELOAD shim for the KeyMint & Gatekeeper HALs
 * ===========================================================================
 *
 * Role in the decrypt stack
 * -------------------------
 * Used for BOTH MITEE HAL services:
 *     /vendor/bin/hw/android.hardware.security.keymint@3.0-service.mitee
 *     /vendor/bin/hw/android.hardware.gatekeeper-service.mitee
 *
 * keymint/gatekeeper link against the system libbinder, so they hit the same
 * VINTF stability checks as servicemanager — we stub those out here too. The
 * shim also traces the TEE round-trips (TEEC_OpenSession / TEEC_InvokeCommand)
 * so we can confirm the MITEE trusted app actually responds (it does: result
 * code 0x0).
 *
 * What it hooks
 * -------------
 *   Stability::requiresVintfDeclaration -> false, Stability::check -> 0
 *   AServiceManager_addService          -> trace (expect 4 OKs from keymint)
 *   TEEC_OpenSession / TEEC_InvokeCommand -> trace TEE result codes
 *   ABinderProcess_joinThreadPool       -> startThreadPool() THEN join, so the
 *       HAL actually has binder threads (needed for linkToDeath)
 *   abort()                             -> kill this thread only (SYS_exit 93)
 *   open/openat                         -> redirect /dev/binder -> newbfs
 *
 * Build:  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 \
 *             -o km_intercept3.so km_intercept3.c
 * ===========================================================================
 */
#define bool int
#include <stddef.h>
#define RTLD_NEXT_KM ((void*)-1L)
extern void* dlsym(void*,const char*) __attribute__((weak));

/* Write a NUL-terminated string straight to stderr via SYS_write (x8=64). */
static void wr(const char* s){
    if(!s||!*s)return;
    const char* e=s;while(*e)e++;
    __asm__ volatile(
        "mov x8,#64\nmov x0,#2\nmov x1,%0\nsub x2,%1,%0\nsvc #0\n"
        ::"r"(s),"r"(e):"x0","x1","x2","x8","memory");}

__attribute__((constructor)) static void init(void){wr("=== km_intercept3 loaded ===\n");}

/* keymint also loads the system libbinder — same VINTF symbols as the SM. */
bool _ZN7android8internal9Stability24requiresVintfDeclarationERKNS_2spINS_7IBinderEEE(void* sp){
    wr("[KM] requiresVintfDeclaration -> false\n");return 0;}

int _ZN7android8internal9Stability5checkEsNS1_5LevelE(short s,int l){
    wr("[KM] check(short,Level) -> 0\n");return 0;}

/* AServiceManager_addService: trace + forward via RTLD_NEXT. */
int AServiceManager_addService(void* binder,const char* name){
    wr("[KM addService] ");if(name)wr(name);wr("\n");
    typedef int(*fn)(void*,const char*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KM,"AServiceManager_addService");
    int r=real?real(binder,name):-1;
    wr(r==0?"[KM addService] -> OK\n":"[KM addService] -> FAIL\n");
    return r;}

/* Mirror the HAL's liblog output. */
int __android_log_buf_write(int b,int p,const char* t,const char* m){
    wr("[KM LOG] ");if(t){wr(t);wr(":");}if(m)wr(m);wr("\n");return 1;}
int __android_log_print(int p,const char* t,const char* fmt,...){
    wr("[KM PRINT] ");if(t){wr(t);wr(":");}if(fmt)wr(fmt);wr("\n");return 1;}

/* TEEC hooks — observe what happens with the TEE trusted app. */
typedef unsigned int TEECR;
TEECR TEEC_OpenSession(void* ctx,void* sess,void* dest,
        unsigned int cm,void* cd,void* op,unsigned int* ro){
    wr("[KM TEEC_Open] called\n");
    typedef TEECR(*fn)(void*,void*,void*,unsigned int,void*,void*,unsigned int*);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KM,"TEEC_OpenSession");
    TEECR r=real?real(ctx,sess,dest,cm,cd,op,ro):0xFFFF0008u;
    char hex[9];int h=7;unsigned int c=r;
    hex[8]=0;do{hex[h--]="0123456789abcdef"[c&0xf];c>>=4;}while(h>=0);
    wr("[KM TEEC_Open] -> 0x");wr(hex);wr("\n");return r;}

TEECR TEEC_InvokeCommand(void* sess,unsigned int cmd,void* op,unsigned int* ro){
    char hex[9];int h=7;unsigned int c=cmd;
    hex[8]=0;do{hex[h--]="0123456789abcdef"[c&0xf];c>>=4;}while(h>=0);
    wr("[KM TEEC_Invoke] cmd=0x");wr(hex);wr("\n");
    typedef TEECR(*fn)(void*,unsigned int,void*,unsigned int*);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KM,"TEEC_InvokeCommand");
    TEECR r=real?real(sess,cmd,op,ro):0xFFFF0008u;
    c=r;h=7;do{hex[h--]="0123456789abcdef"[c&0xf];c>>=4;}while(h>=0);
    wr("[KM TEEC_Invoke] -> 0x");wr(hex);wr("\n");return r;}

/* Fix: keymint needs live binder threads for linkToDeath. The stock service
 * calls only joinThreadPool(); start the pool first so threads exist. */
void ABinderProcess_joinThreadPool(void){
    wr("[KM] starting thread pool before join\n");
    typedef void(*fn)(void);
    static fn rs=0,rj=0;
    if(!rs&&dlsym)rs=(fn)dlsym((void*)-1L,"ABinderProcess_startThreadPool");
    if(!rj&&dlsym)rj=(fn)dlsym((void*)-1L,"ABinderProcess_joinThreadPool");
    if(rs)rs();
    if(rj)rj();
}

__attribute__((noreturn)) void abort(void){
    wr("[KM] abort intercepted\n");
    register long x8 __asm__("x8")=93,x0 __asm__("x0")=1;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory");
    __builtin_unreachable();}

/* Redirect /dev/binder -> /dev/newbfs/binder (our private binderfs). */
static int beq(const char* a,const char* b){
    if(!a||!b)return 0;
    int i=0;while(a[i]&&b[i]&&a[i]==b[i])i++;
    return !a[i]&&!b[i];}

static const char* rbind(const char* p){
    if(beq(p,"/dev/binder")||beq(p,"/dev/binderfs/binder")){
        wr("[REDIR] binder -> /dev/newbfs/binder\n");
        return "/dev/newbfs/binder";}
    return p;}

int open(const char* p,int f,...){
    typedef int(*fn)(const char*,int,...);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"open");
    p=rbind(p);
    if(f&0100){void* m;__asm__ volatile("ldr %0,[sp]":"=r"(m));return r?r(p,f,m):-1;}
    return r?r(p,f):-1;}

int openat(int d,const char* p,int f,...){
    typedef int(*fn)(int,const char*,int,...);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"openat");
    p=rbind(p);
    if(f&0100){void* m;__asm__ volatile("ldr %0,[sp]":"=r"(m));return r?r(d,p,f,m):-1;}
    return r?r(d,p,f):-1;}
