/* ===========================================================================
 * sm_stab3.c  —  LD_PRELOAD shim for Android's `servicemanager`
 * ===========================================================================
 *
 * Role in the decrypt stack
 * -------------------------
 * To decrypt FBE userdata we need a working binder Service Manager running
 * inside TWRP so that keymint/gatekeeper/keystore2 can register and find each
 * other. The stock servicemanager binary (pulled from the system_a image) does
 * not come up cleanly under recovery, so this shim patches around the failures:
 *
 *   - Stability::requiresVintfDeclaration -> false, Stability::check -> 0
 *       Bypass the VINTF stability checks so unblessed services can register.
 *   - VintfObject::fetchDeviceHalManifest
 *       The APEX manifest overlays fail with ELOOP (-40) in recovery; we force
 *       the result to OK and rely on the vendor manifest (which already lists
 *       keymint), so the SM accepts the partial manifest instead of aborting.
 *   - ioctl(BINDER_SET_CONTEXT_MGR)
 *       Becoming the context manager races the still-dying init servicemanager;
 *       retry up to 120x500ms until it succeeds, then immediately publish
 *       `servicemanager.ready=true`.
 *   - __system_property_set("servicemanager.ready","false")
 *       BLOCKED — the SM tries to clear this at startup, which would make every
 *       client think no SM exists.
 *   - abort() -> spin (wfi) instead of dying.
 *   - open/openat/stat/lstat
 *       Redirect /dev/binder -> /dev/newbfs/binder (our private binderfs) and
 *       log any ELOOP so we can find which path the VINTF loader chokes on.
 *
 * Build:  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 \
 *             -o sm_stab3.so sm_stab3.c
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

__attribute__((constructor)) static void init(void){wr("=== sm_stab3 loaded ===\n");}

/* Mirror the SM's own liblog output to our stderr log. */
int __android_log_buf_write(int b,int p,const char* t,const char* m){
    wr("[SM LOG] ");if(t){wr(t);wr(":");}if(m)wr(m);wr("\n");return 1;}
int __android_log_print(int p,const char* t,const char* fmt,const char* arg1,...){
    wr("[SM LOG] ");if(t){wr(t);wr(":");}if(fmt)wr(fmt);
    if(arg1&&(unsigned long)arg1>4096UL){wr(" -> ");wr(arg1);}
    wr("\n");return 1;}

/* android::internal::Stability::requiresVintfDeclaration(sp<IBinder>) -> false
 * android::internal::Stability::check(short, Level)                  -> 0
 * Let services register without a matching VINTF declaration. */
bool _ZN7android8internal9Stability24requiresVintfDeclarationERKNS_2spINS_7IBinderEEE(void* sp){
    (void)sp;return 0;}
int _ZN7android8internal9Stability5checkEsNS1_5LevelE(short s,int l){
    (void)s;(void)l;return 0;}

extern void* dlsym(void*,const char*) __attribute__((weak));

static void sleep_500ms(void){
    long ts[2]={0,500000000L};
    register long x8 __asm__("x8")=101;
    register long x0 __asm__("x0")=(long)ts;
    register long x1 __asm__("x1")=0;
    __asm__ volatile("svc #0":"+r"(x0),"+r"(x1):"r"(x8):"memory");}

/* ioctl: retry BINDER_SET_CONTEXT_MGR until we win the context-manager role,
 * then immediately set servicemanager.ready=true. The two request codes are
 * BINDER_SET_CONTEXT_MGR and BINDER_SET_CONTEXT_MGR_EXT. */
int ioctl(int fd,unsigned long req,long arg){
    typedef int(*fn)(int,unsigned long,long);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"ioctl");
    if(!real)return -1;
    int r=real(fd,req,arg);
    if((req==0x40046207ul||req==0x4018620dul)&&r!=0){
        wr("[SM] CM failed, retrying...\n");
        int i=0;
        while(r!=0&&i<120){sleep_500ms();r=real(fd,req,arg);i++;}
        if(r==0){
            wr("[SM] became CM!\n");
            /* Publish servicemanager.ready=true right away. */
            typedef int(*pset_t)(const char*,const char*);
            static pset_t pset=0;
            if(!pset&&dlsym)pset=(pset_t)dlsym((void*)-1L,"__system_property_set");
            if(pset){pset("servicemanager.ready","true");
                     wr("[SM] servicemanager.ready=true set\n");}
        }else wr("[SM] CM GAVE UP\n");}
    return r;}

__attribute__((noreturn)) void abort(void){
    wr("[SM] ABORT - spinning\n");
    while(1){__asm__ volatile("wfi":::"memory");}
    __builtin_unreachable();}

/* VintfObject::fetchDeviceHalManifest(HalManifest*, string*)
 * Returns ELOOP (-40) in recovery because the APEX VINTF overlays loop on
 * themselves. The vendor manifest is already loaded (and lists keymint), so we
 * force the result to 0 and proceed with the partial manifest. */
int _ZN7android5vintf11VintfObject22fetchDeviceHalManifestEPNS0_11HalManifestEPNSt3__112basic_stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEE(void*self,void*man,void*err){
    wr("[SM] fetchDeviceHalManifest enter\n");
    typedef int(*fdm_t)(void*,void*,void*);static fdm_t real=0;
    if(!real)real=(fdm_t)dlsym((void*)-1L,
        "_ZN7android5vintf11VintfObject22fetchDeviceHalManifestEPNS0_11HalManifestEPNSt3__112basic_stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEE");
    int r=real?real(self,man,err):-99;
    if(r==0){
        wr("[SM] fetchDeviceHalManifest OK\n");
    }else if(r==-40){
        /* ELOOP: vendor manifest already loaded (keymint is there), the APEX
           overlays failed. Return 0 — use the partial manifest. */
        wr("[SM] fetchDeviceHalManifest ELOOP->FORCING OK\n");
        r=0;
    }else{
        wr("[SM] fetchDeviceHalManifest FAIL r=");wrint(r);
    }
    return r;}

/* ─── string helpers ─── */
static int beq(const char* a,const char* b){
    if(!a||!b)return 0;int i=0;
    while(a[i]&&b[i]&&a[i]==b[i])i++;return !a[i]&&!b[i];}

static int sw(const char* s,const char* p){
    if(!s||!p)return 0;int i=0;
    while(p[i]&&s[i]&&p[i]==s[i])i++;return !p[i];}

static const char* rbind(const char* p){
    /* NATIVE-CONTEXT MODE: do NOT redirect to a private binderfs. recovery_real
     * already holds /dev/binderfs/binder open (fd opened at boot); using a
     * separate newbfs context puts us on a DIFFERENT binder context, so TWRP's
     * service lookups never see what we register. Open the native binder so our
     * SM becomes context manager of the very context TWRP is already on. */
    (void)beq;
    return p;}

/* Paths worth tracing while hunting the ELOOP source. */
static int interesting(const char* p){
    if(!p)return 0;
    return sw(p,"/vendor")||sw(p,"/apex")||sw(p,"/odm")||
           sw(p,"/product")||sw(p,"/system_ext")||
           sw(p,"/system/etc/vintf");}

/* Raw openat syscall — returns -errno on error (used only to read errno). */
static long sys_openat(int d,const char* p,int f){
    register long x8 __asm__("x8")=56;
    register long x0 __asm__("x0")=(long)(unsigned)d;
    register long x1 __asm__("x1")=(long)p;
    register long x2 __asm__("x2")=(long)(f&~0100);
    register long x3 __asm__("x3")=0;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
    return x0;}

static void sys_close(long fd){
    register long x8 __asm__("x8")=57;
    register long x0 __asm__("x0")=fd;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory");}


/* ─── Block the SM from clearing servicemanager.ready at startup ─── */
int __system_property_set(const char* name,const char* value){
    typedef int(*fn)(const char*,const char*);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"__system_property_set");
    if(!real)return -1;
    /* Don't let the SM set servicemanager.ready=false during init. */
    if(name&&beq(name,"servicemanager.ready")&&value&&value[0]=='f'){
        wr("[SM] BLOCKED prop servicemanager.ready=false\n");
        return 0;}
    return real(name,value);}

/* ─── open: redirect binder, trace interesting opens ─── */
int open(const char* p,int f,...){
    typedef int(*fn)(const char*,int,...);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"open");
    const char* rp=rbind(p);
    int fd=-1;
    if(f&0100){void* m;__asm__ volatile("ldr %0,[sp]":"=r"(m));fd=r?r(rp,f,m):-1;}
    else fd=r?r(rp,f):-1;
    if(interesting(p)){wr("[SM OPEN] ");wr(p);wr(fd<0?" FAIL\n":" OK\n");}
    return fd;}

/* ─── openat: log ANY failure with errno; always log interesting paths ─── */
static int oat_init=0;
int openat(int d,const char* p,int f,...){
    typedef int(*fn)(int,const char*,int,...);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"openat");
    if(!oat_init){oat_init=1;wr("[SM OAT] hook active\n");}
    const char* rp=rbind(p);
    int fd=-1;
    if(f&0100){void* m;__asm__ volatile("ldr %0,[sp]":"=r"(m));fd=r?r(d,rp,f,m):-1;}
    else fd=r?r(d,rp,f):-1;

    if(fd<0){
        long e=sys_openat(d,rp,f);
        if(e>=0)sys_close(e);
        /* Always log ELOOP (-40), regardless of path. */
        if(e==-40){wr("[SM ELOOP openat] p=");wr(p?p:"(null)");wr("\n");}
        /* Interesting paths — log with errno. */
        else if(interesting(p)){wr("[SM OAT FAIL] ");wr(p);wr(" errno=");wrint(e);}
    }else if(interesting(p)){wr("[SM OAT OK] ");wr(p);wr("\n");}
    return fd;}

/* ─── stat: ELOOP often surfaces here rather than from openat ─── */
/* fstatat(AT_FDCWD, path, buf, 0) = syscall 79 on aarch64 */
static long sys_fstatat(int dfd,const char* p,void* buf,int flags){
    register long x8 __asm__("x8")=79;  /* __NR_newfstatat */
    register long x0 __asm__("x0")=(long)(unsigned)dfd;
    register long x1 __asm__("x1")=(long)p;
    register long x2 __asm__("x2")=(long)buf;
    register long x3 __asm__("x3")=(long)flags;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
    return x0;}

static int stat_init=0;
int stat(const char* p,void* buf){
    typedef int(*fn)(const char*,void*);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"stat");
    if(!stat_init){stat_init=1;wr("[SM STAT] hook active\n");}
    int ret=r?r(p,buf):-1;
    if(ret<0){
        /* Read errno via a direct syscall. */
        char tmp[512];  /* stat64 buffer */
        long e=sys_fstatat(-100,p,tmp,0); /* AT_FDCWD=-100 */
        if(e==-40){wr("[SM ELOOP stat] ");wr(p?p:"(null)");wr("\n");}
        else if(interesting(p)){wr("[SM STAT FAIL] ");wr(p);wr(" errno=");wrint(e);}
    }
    return ret;}

int lstat(const char* p,void* buf){
    typedef int(*fn)(const char*,void*);
    static fn r=0;if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"lstat");
    int ret=r?r(p,buf):-1;
    if(ret<0){
        char tmp[512];
        long e=sys_fstatat(-100,p,tmp,0x0100); /* AT_SYMLINK_NOFOLLOW=0x100 */
        if(e==-40){wr("[SM ELOOP lstat] ");wr(p?p:"(null)");wr("\n");}
    }
    return ret;}
