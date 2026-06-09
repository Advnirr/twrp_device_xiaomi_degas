/* ===========================================================================
 * ks2_log.c  —  LD_PRELOAD shim for `keystore2`
 * ===========================================================================
 *
 * Role in the decrypt stack
 * -------------------------
 * keystore2 is the AIDL service TWRP talks to in order to unwrap the FBE key.
 * Started under recovery it tends to hang forever waiting for services that do
 * not exist there. This shim makes it come up and register cleanly:
 *
 *   - is_nonexistent_in_recovery()
 *       AServiceManager_waitForService() for apexd / strongbox would block the
 *       keystore2 worker thread forever (those services never appear in
 *       recovery). Return NULL immediately instead. This also avoids a multi-
 *       minute watchdog freeze on IApexService.
 *   - AServiceManager_isDeclared -> FORCED TRUE for keymint/sharedsecret/
 *       gatekeeper, so keystore2 will actually try to connect to them.
 *   - forEachDeclaredInstance: after the real call, actively probe
 *       waitForService("<keymint iface>/default") so we can confirm in the log
 *       that keystore2 can reach keymint (PROBE_WFS -> FOUND).
 *   - Plus tracing for waitForService/checkService/getService/addService and
 *       AIBinder_associateClass / getDescriptor.
 *   - abort() -> kill this thread only (SYS_exit 93), not the whole process.
 *   - open/openat -> redirect /dev/binder -> /dev/newbfs/binder.
 *
 * Build:  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 \
 *             -o ks2_log.so ks2_log.c
 * ===========================================================================
 */
#define bool int
#include <stddef.h>

#define RTLD_NEXT_KS ((void*)-1L)
extern void* dlsym(void*,const char*) __attribute__((weak));

/* Write a NUL-terminated string straight to stderr via SYS_write (x8=64). */
static void wr(const char* s){
    if(!s||!*s)return;
    const char* e=s;while(*e)e++;
    __asm__ volatile(
        "mov x8,#64\nmov x0,#2\nmov x1,%0\nsub x2,%1,%0\nsvc #0\n"
        ::"r"(s),"r"(e):"x0","x1","x2","x8","memory");}

static int startswith(const char* s,const char* p){
    if(!s||!p)return 0;
    int i=0;while(p[i]&&s[i]&&p[i]==s[i])i++;return !p[i];}

__attribute__((constructor)) static void init(void){wr("=== ks2_log loaded ===\n");}

/* Mirror keystore2's liblog output to our stderr log. */
int __android_log_buf_write(int b,int p,const char* t,const char* m){
    wr("[B]");if(t){wr(t);wr(":");}if(m)wr(m);wr("\n");return 1;}
int __android_log_write(int p,const char* t,const char* m){
    wr("[W]");if(t){wr(t);wr(":");}if(m)wr(m);wr("\n");return 1;}
int __android_log_print(int p,const char* t,const char* fmt,...){
    wr("[P]");if(t){wr(t);wr(":");}if(fmt)wr(fmt);wr("\n");return 1;}

__attribute__((noreturn)) void abort(void){
    wr("[KS2] ABORT - killing thread only\n");
    register long x8 __asm__("x8")=93,x0 __asm__("x0")=1;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory");}

/* ── Services that will never appear in recovery → return NULL immediately ──
   Otherwise AServiceManager_waitForService blocks the thread forever.        */
static int is_nonexistent_in_recovery(const char* n){
    if(!n)return 0;
    /* apexd is not running in recovery */
    if(startswith(n,"android.apex.IApexService"))         return 1;
    /* strongbox — not present on this device */
    if(startswith(n,"android.hardware.security.sharedsecret.ISharedSecret/strongbox")) return 1;
    if(startswith(n,"android.hardware.keymaster@4.1::IKeymasterDevice/strongbox"))     return 1;
    if(startswith(n,"android.hardware.security.keymint.IKeyMintDevice/strongbox"))     return 1;
    return 0;}

void* AServiceManager_waitForService(const char* n){
    wr("[KS2 WFS] ");if(n)wr(n);wr("\n");
    if(is_nonexistent_in_recovery(n)){
        wr("[KS2 WFS] -> NULL (skip: not present in recovery)\n");
        return 0;}
    typedef void*(*fn)(const char*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KS,"AServiceManager_waitForService");
    void* r=real?real(n):0;
    wr(r?"[KS2 WFS] -> FOUND\n":"[KS2 WFS] -> NULL\n");return r;}

void* AServiceManager_checkService(const char* n){
    wr("[KS2 CHK] ");if(n)wr(n);wr("\n");
    typedef void*(*fn)(const char*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KS,"AServiceManager_checkService");
    void* r=real?real(n):0;
    wr(r?"[KS2 CHK] -> FOUND\n":"[KS2 CHK] -> NULL\n");return r;}

/* KEY PATCH: isDeclared -> true for keymint/sharedsecret/gatekeeper, so that
 * keystore2 attempts to connect to the HALs we started by hand. */
bool AServiceManager_isDeclared(const char* n){
    wr("[KS2 DECL] ");if(n)wr(n);wr("\n");
    if(n&&(startswith(n,"android.hardware.security.keymint")||
           startswith(n,"android.hardware.security.sharedsecret")||
           startswith(n,"android.hardware.gatekeeper"))){
        wr("[KS2 DECL] -> FORCED TRUE\n");return 1;}
    typedef bool(*fn)(const char*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KS,"AServiceManager_isDeclared");
    bool r=real?real(n):0;
    wr(r?"[KS2 DECL] -> true\n":"[KS2 DECL] -> false\n");return r;}

int AServiceManager_addService(void* binder,const char* name){
    wr("[KS2 ADD] "); if(name)wr(name); wr("\n");
    typedef int(*fn)(void*,const char*); static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"AServiceManager_addService");
    int r=real?real(binder,name):-1;
    wr(r==0?"[KS2 ADD] -> OK\n":"[KS2 ADD] -> FAIL\n");
    return r;}

void* AServiceManager_getService(const char* n){
    wr("[KS2 GET] ");if(n)wr(n);wr("\n");
    typedef void*(*fn)(const char*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KS,"AServiceManager_getService");
    void* r=real?real(n):0;
    wr(r?"[KS2 GET] -> FOUND\n":"[KS2 GET] -> NULL\n");return r;}

/* When keystore2 enumerates keymint instances, immediately probe the well-known
 * "<iface>/default" via waitForService so the log proves the link works. */
void AServiceManager_forEachDeclaredInstance(const char* iface,void* ctx,void(*fn)(const char*,void*)){
    wr("[KS2 EACH] ");if(iface)wr(iface);wr("\n");
    typedef void(*rfn)(const char*,void*,void(*)(const char*,void*));static rfn real=0;
    if(!real&&dlsym)real=(rfn)dlsym(RTLD_NEXT_KS,"AServiceManager_forEachDeclaredInstance");
    if(real)real(iface,ctx,fn);
    if(iface&&startswith(iface,"android.hardware.security.keymint.IKeyMintDevice")){
        char fname[200];int i=0;
        while(iface[i]&&i<180){fname[i]=iface[i];i++;}
        const char* sfx="/default";int j=0;
        while(sfx[j]){fname[i++]=sfx[j++];}fname[i]=0;
        wr("[KS2 PROBE_WFS] ");wr(fname);wr("\n");
        typedef void*(*wfn)(const char*);static wfn rw=0;
        if(!rw&&dlsym)rw=(wfn)dlsym(RTLD_NEXT_KS,"AServiceManager_waitForService");
        void* rb=rw?rw(fname):0;
        wr(rb?"[KS2 PROBE_WFS] -> FOUND\n":"[KS2 PROBE_WFS] -> NULL\n");}}

bool AIBinder_associateClass(void* binder, const void* clazz){
    wr("[KS2 ASSOC] called\n");
    typedef bool(*fn)(void*, const void*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KS,"AIBinder_associateClass");
    bool r=real?real(binder,clazz):0;
    wr(r?"[KS2 ASSOC] -> true\n":"[KS2 ASSOC] -> false\n");return r;}

const char* AIBinder_getDescriptor(const void* binder){
    typedef const char*(*fn)(const void*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_KS,"AIBinder_getDescriptor");
    const char* r=real?real(binder):0;
    wr("[KS2 DESC] ");wr(r?r:"null");wr("\n");return r;}

static int beq(const char* a,const char* b){
    if(!a||!b)return 0;int i=0;
    while(a[i]&&b[i]&&a[i]==b[i])i++;return !a[i]&&!b[i];}

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
