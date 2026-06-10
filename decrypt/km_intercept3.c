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

/* ---------------------------------------------------------------------------
 * PROPERTY OVERRIDE  —  fix the KeyMint version-binding (-33 INVALID_KEY_BLOB).
 *
 * The recovery ramdisk reports stale build props (security_patch=2022-04-05,
 * release=12, no vendor patch). keymaster::GetOs{Version,Patchlevel} and
 * GetVendorPatchlevel read these and the HAL pushes them into the MiTEE TA,
 * which then binds/enforces them on every key. The /data key blob was bound to
 * the REAL normal-boot values, so a mismatch makes the TA reject the blob.
 *
 * We override the handful of props the patchlevel helpers read so the TA is
 * configured with the same values normal boot uses. libbase::GetProperty goes
 * through __system_property_find + __system_property_read_callback; libcutils
 * property_get goes through __system_property_get. We intercept all three.
 * ------------------------------------------------------------------------- */
static int km_streq(const char* a,const char* b){
    if(!a||!b)return 0;int i=0;while(a[i]&&a[i]==b[i])i++;return a[i]==b[i];}

static const char* const FAKE_N[4]={
    "ro.build.version.security_patch",
    "ro.build.version.release",
    "ro.vendor.build.security_patch",
    "ro.boot.security_patch"};
static const char* const FAKE_V[4]={"2026-02-01","16","2026-02-01","2026-02-01"};
static char fake_pi[4];   /* sentinel prop_info storage (addresses are tokens) */

typedef void (*prop_cb)(void* cookie,const char* name,const char* value,unsigned int serial);

const void* __system_property_find(const char* name){
    typedef const void*(*fn)(const char*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"__system_property_find");
    for(int i=0;i<4;i++) if(km_streq(name,FAKE_N[i])){
        wr("[KM prop-find] faking ");wr(name);wr("\n");
        return (const void*)&fake_pi[i];}
    return real?real(name):0;}

void __system_property_read_callback(const void* pi,prop_cb cb,void* cookie){
    if(pi>=(const void*)&fake_pi[0] && pi<=(const void*)&fake_pi[3]){
        int i=(int)((const char*)pi-&fake_pi[0]);
        wr("[KM prop-read] -> ");wr(FAKE_V[i]);wr("\n");
        if(cb)cb(cookie,FAKE_N[i],FAKE_V[i],1);
        return;}
    typedef void(*fn)(const void*,prop_cb,void*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"__system_property_read_callback");
    if(real)real(pi,cb,cookie);}

int __system_property_get(const char* name,char* value){
    for(int i=0;i<4;i++) if(km_streq(name,FAKE_N[i])){
        int j=0;while(FAKE_V[i][j]){value[j]=FAKE_V[i][j];j++;}value[j]=0;
        wr("[KM prop-get] ");wr(name);wr(" -> ");wr(value);wr("\n");
        return j;}
    typedef int(*fn)(const char*,char*);static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"__system_property_get");
    return real?real(name,value):0;}

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
    /* NATIVE-CONTEXT MODE: no redirect — share the native /dev/binderfs/binder
     * context that recovery_real and our servicemanager use. See sm_stab3.c. */
    (void)beq;
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
