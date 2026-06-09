/* ===========================================================================
 * throw_logger_alt.c  —  alternate LD_PRELOAD shim for `recovery_real`
 * ===========================================================================
 *
 * An earlier sibling of twrp_fix.c (see that file for the main approach). The
 * difference worth keeping: its abort() handler walks the AArch64 frame pointer
 * chain (x29) and dumps up to 12 return addresses BEFORE exiting the thread, so
 * you get a cheap stack backtrace of whatever called abort() — handy when you
 * don't yet know which library is dying.
 *
 * It also resolves AServiceManager_* out of libbinder_ndk.so explicitly via
 * dlopen (instead of RTLD_NEXT), and stubs std::thread destructors to avoid a
 * teardown crash.
 *
 * Both this file and twrp_fix.c are implementations of the recovery_real shim
 * and are deployed on-device as the same `/system/bin/throw_logger.so`.
 * twrp_fix.c is the current/primary one (thread-only exit + F2FS/DM tracing,
 * used in the manual deploy in docs/RESEARCH.md → Next steps); this alt is kept
 * for its abort() backtrace. patch_touch.sh bakes whichever you've built as
 * throw_logger.so into the image; from_scratch.sh deploys neither.
 *
 * Build:  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 \
 *             -o throw_logger_alt.so throw_logger_alt.c
 * ===========================================================================
 */
#define SYS_write  64
#define SYS_exit   93
void* dlopen(const char*, int);
void* dlsym(void*, const char*);

static long _write(int fd,const void* b,unsigned long l){
    register long x8 __asm__("x8")=SYS_write,x0 __asm__("x0")=fd;
    register const void* x1 __asm__("x1")=b;register unsigned long x2 __asm__("x2")=l;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x8):"memory");return x0;}
static void wr(const char* s){unsigned long n=0;while(s[n])n++;_write(2,s,n);}
static void wr_hex(unsigned long v){
    char b[19]="0x0000000000000000\n";const char* h="0123456789abcdef";
    for(int i=17;i>=2;i--){b[i]=h[v&0xf];v>>=4;}_write(2,b,19);}

static void* get_fn(const char* sym) {
    static void* h = 0;
    if(!h){
        h = dlopen("/vendor/lib64/libbinder_ndk.so", 6);
        if(!h) h = dlopen("/system/lib64/libbinder_ndk.so", 6);
        if(!h) h = dlopen("libbinder_ndk.so", 2);
    }
    return h ? dlsym(h, sym) : 0;
}

int AServiceManager_addService(void* binder, const char* name) {
    wr("[SM+] addService("); if(name)wr(name); wr(")\n");
    typedef int(*fn)(void*,const char*);
    fn real = (fn)get_fn("AServiceManager_addService");
    int r = real ? real(binder, name) : -1;
    if(r==0) wr("[SM+] addService -> OK\n");
    else { wr("[SM+] addService -> ERR "); wr_hex((unsigned long)(unsigned int)r); }
    return r;
}
void ABinderProcess_joinThreadPool(void) {
    wr("[SM+] joinThreadPool\n");
    typedef void(*fn)(void);
    fn real = (fn)get_fn("ABinderProcess_joinThreadPool");
    if(real) real();
}
int __android_log_write(int p,const char* t,const char* m){
    const char* lv="XXVDIWEF";char pr[4]={'[','?',']',' '};
    if(p>=0&&p<8)pr[1]=lv[p];_write(2,pr,4);if(t)wr(t);wr(": ");if(m)wr(m);wr("\n");return 0;}
int __android_log_buf_write(int b,int p,const char* t,const char* m){return __android_log_write(p,t,m);}
int puts(const char* s){wr("[P] ");if(s)wr(s);wr("\n");return 1;}
void _ZNSt3__16threadD2Ev(void* s){}
void _ZNSt3__16threadD1Ev(void* s){}
void abort(void){
    wr(">>> ABORT:\n");unsigned long* fp;
    __asm__ volatile("mov %0, x29":"=r"(fp));
    for(int i=0;i<12;i++){
        if(!fp||(unsigned long)fp<0x1000)break;
        char n[4]={'#','0'+i/10,'0'+i%10,' '};_write(2,n,4);wr_hex(fp[1]);
        unsigned long* nx=(unsigned long*)fp[0];if(nx<=fp)break;fp=nx;}
    register long x8 __asm__("x8")=SYS_exit,x0r __asm__("x0")=1;
    __asm__ volatile("svc #0"::"r"(x8),"r"(x0r):"memory");__builtin_unreachable();}
__attribute__((constructor)) static void init(void){wr("=== throw_logger v6 ===\n");}

/* AServiceManager_waitForService logging */
#define RTLD_NEXT_TL ((void*)-1L)
extern void* dlsym(void*,const char*) __attribute__((weak));
void* AServiceManager_waitForService(const char* name){
    wr("[WFS] ");wr(name?name:"null");wr("\n");
    typedef void*(*fn)(const char*);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_TL,"AServiceManager_waitForService");
    void* r=real?real(name):0;
    wr(r?"[WFS] -> FOUND\n":"[WFS] -> NULL\n");
    return r;}
void* AServiceManager_checkService(const char* name){
    wr("[CKS] ");wr(name?name:"null");wr("\n");
    typedef void*(*fn)(const char*);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym(RTLD_NEXT_TL,"AServiceManager_checkService");
    void* r=real?real(name):0;
    wr(r?"[CKS] -> FOUND\n":"[CKS] -> NULL\n");
    return r;}
