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
    if(t==0xfd && arg){
        unsigned int* v=(unsigned int*)arg;        /* dm_ioctl */
        const char* nm=(const char*)arg+48;        /* name[] at off 48 */
        wr("[DM] nr=0x");wrhex2(n);wr(" ver=");wrhex(v[0]);
        wr(" flags=");wrhex(v[7]);wr(" name='");wr(nm);wr("'\n");
    }
    int r=real(fd,req,arg);
    if(r<0&&t==0xf5){wr("[twrp_fix] F2FS->OK\n");return 0;}
    if(r<0&&t==0xfd){
        /* errno is in x0 as -errno from the raw syscall path; libc set it. Show
         * the returned value so we can see EINVAL(-22) vs EBUSY(-16) etc. */
        wr("[DM FAIL] nr=0x");wrhex2(n);wr(" r=");wrint(r);}
    else if(r==0&&t==0xfd){wr("[DM OK] nr=0x");wrhex2(n);wr("\n");}
    return r;}

/* std::terminate — log the return address (so we can identify which library
 * threw) and kill only this thread. */
void _ZSt9terminatev(void){
    wr("[twrp_fix] std::terminate BT:\n");
    unsigned long fp;
    __asm__ volatile("mov %0, x29":"=r"(fp));
    for(int i=0;i<16 && fp && !(fp&7);i++){
        unsigned long lr=*(unsigned long*)(fp+8);
        unsigned long nf=*(unsigned long*)(fp);
        wr("  #");wrhex(lr);wr("\n");
        if(nf<=fp)break;
        fp=nf;
    }
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

/* 250ms nanosleep via SYS_nanosleep (x8=101). */
static void nsleep_250ms(void){
    long ts[2]={0,250000000L};
    register long x8 __asm__("x8")=101;
    register long x0 __asm__("x0")=(long)ts;
    register long x1 __asm__("x1")=0;
    __asm__ volatile("svc #0":"+r"(x0),"+r"(x1):"r"(x8):"memory");}

/* Force the CLIENT-side binder stability check to pass. Disassembly of TWRP's
 * BpBinder::transact shows it calls Stability::check(category, level) via the
 * PLT and, if it returns with bit0 == 0 (incompatible), bails out with BAD_TYPE
 * (0x80000001) WITHOUT ever sending the transaction. Our handle-0 SM proxy's
 * category doesn't match recovery's local level, which is exactly why the
 * bridge's code-4 transact never reached the driver. Because the call goes
 * through the PLT, LD_PRELOAD can interpose it; returning 1 (compatible) lets
 * every transaction through — fine for our recovery sandbox. */
int _ZN7android8internal9Stability5checkENS1_8CategoryENS1_5LevelE(int cat,int lvl){
    (void)cat;(void)lvl;return 1;}

/* ============================================================================
 * Service-lookup bridge around TWRP's broken libbinder.
 *
 * Byte-diffing TWRP's `[TWRP WFS] NULL` lookup against the stock `service check`
 * (which finds the service fine) showed the request parcel is identical, but:
 *   - TWRP's IServiceManager proxy uses the OLD AIDL transaction numbering
 *     (getService=1, checkService=2); the stock Android-16 servicemanager puts
 *     the working lookup at code 4.
 *   - code 4 returns a RICH reply (a Service parcelable): [int32 status][16B
 *     wrapper][flat_binder_object @ offset 20][int32 stability]. TWRP's
 *     checkService parses it as a plain IBinder and reads the wrapper -> NULL.
 * The reply buffer is mmapped read-only (mprotect -> EACCES), so it can't be
 * reshaped in place inside the ioctl hook.
 *
 * So we perform the lookup ourselves via TWRP's own libbinder C++ API: build the
 * exact request parcel, BpBinder::transact(code 4) on the handle-0 proxy, move
 * the reply cursor to the flat_binder_object at offset 20, readStrongBinder()
 * (libbinder owns the ref-counting + buffer free), and convert the platform
 * sp<IBinder> to an NDK AIBinder* via AIBinder_fromPlatformBinder. All symbols
 * confirmed present in /tw_libs/libbinder*.so.
 * ============================================================================ */

/* Generic AArch64 call: integer args in x0..x4, sret pointer in x8; returns x0.
 * x18 (Android platform reg) is deliberately NOT clobbered. */
static long bcall(void* fn,long a0,long a1,long a2,long a3,long a4,void* x8p){
    register long r0 __asm__("x0")=a0;
    register long r1 __asm__("x1")=a1;
    register long r2 __asm__("x2")=a2;
    register long r3 __asm__("x3")=a3;
    register long r4 __asm__("x4")=a4;
    register long r8 __asm__("x8")=(long)x8p;
    register long r9 __asm__("x9")=(long)fn;
    __asm__ volatile("blr x9"
        :"+r"(r0),"+r"(r1),"+r"(r2),"+r"(r3),"+r"(r4),"+r"(r8),"+r"(r9)
        :
        :"x5","x6","x7","x10","x11","x12","x13","x14","x15","x16","x17",
         "x30","cc","memory",
         "v0","v1","v2","v3","v4","v5","v6","v7");
    return r0;}

/* Append an AIDL String16: int32 len, then len+1 UTF-16 units (incl NUL), pad 4. */
static unsigned long put_str16(unsigned char* b,unsigned long off,const char* s){
    unsigned int len=0; while(s&&s[len])len++;
    *(unsigned int*)(b+off)=len; off+=4;
    for(unsigned int i=0;i<len;i++){b[off]=(unsigned char)s[i];b[off+1]=0;off+=2;}
    b[off]=0;b[off+1]=0;off+=2;
    while(off&3){b[off]=0;off++;}
    return off;}

/* Look up `name` in the SM, returning an AIBinder* (or 0). */
static void* bridge_lookup(const char* n){
    static int done=0;
    static void *f_self,*f_gsp,*f_pctor,*f_pdtor,*f_write,*f_transact,
                *f_setpos,*f_readsb,*f_fromplat,*f_errchk,*f_dsize,*f_data;
    if(!done){
        done=1;
        if(dlsym){
            f_self    =dlsym((void*)-1L,"_ZN7android12ProcessState4selfEv");
            f_gsp     =dlsym((void*)-1L,"_ZN7android12ProcessState23getStrongProxyForHandleEi");
            f_pctor   =dlsym((void*)-1L,"_ZN7android6ParcelC1Ev");
            f_pdtor   =dlsym((void*)-1L,"_ZN7android6ParcelD1Ev");
            f_write   =dlsym((void*)-1L,"_ZN7android6Parcel5writeEPKvm");
            f_transact=dlsym((void*)-1L,"_ZN7android8BpBinder8transactEjRKNS_6ParcelEPS1_j");
            f_setpos  =dlsym((void*)-1L,"_ZNK7android6Parcel15setDataPositionEm");
            f_readsb  =dlsym((void*)-1L,"_ZNK7android6Parcel16readStrongBinderEv");
            f_fromplat=dlsym((void*)-1L,"_Z27AIBinder_fromPlatformBinderRKN7android2spINS_7IBinderEEE");
            f_errchk  =dlsym((void*)-1L,"_ZNK7android6Parcel10errorCheckEv");
            f_dsize   =dlsym((void*)-1L,"_ZNK7android6Parcel8dataSizeEv");
            f_data    =dlsym((void*)-1L,"_ZNK7android6Parcel4dataEv");
        }
    }
    if(!f_self||!f_gsp||!f_pctor||!f_pdtor||!f_write||!f_transact||
       !f_setpos||!f_readsb||!f_fromplat){wr("[BRIDGE] missing symbols\n");return 0;}

    /* request parcel: strictPolicy, workSource(-1), kHeader 'SYST', token, name */
    unsigned char req[600];
    unsigned long rlen=0;
    *(unsigned int*)(req+rlen)=0x80000000u; rlen+=4;
    *(unsigned int*)(req+rlen)=0xFFFFFFFFu; rlen+=4;
    *(unsigned int*)(req+rlen)=0x53595354u; rlen+=4;
    rlen=put_str16(req,rlen,"android.os.IServiceManager");
    rlen=put_str16(req,rlen,n);

    /* Parcel data, reply — zeroed, 16-byte aligned stack storage (Parcel has
     * pointer members), generous for the object size. */
    __attribute__((aligned(16))) unsigned char data[512];
    __attribute__((aligned(16))) unsigned char reply[512];
    for(int i=0;i<512;i++){data[i]=0;reply[i]=0;}
    bcall(f_pctor,(long)data,0,0,0,0,0);
    bcall(f_pctor,(long)reply,0,0,0,0,0);
    long w=bcall(f_write,(long)data,(long)req,(long)rlen,0,0,0);

    void* psp[2]={0,0};
    bcall(f_self,0,0,0,0,0,psp);                  /* sp<ProcessState> */
    void* smsp[2]={0,0};
    bcall(f_gsp,(long)psp[0],0,0,0,0,smsp);        /* getStrongProxyForHandle(0) */
    void* sm=smsp[0];
    (void)w;
    void* result=0;
    if(sm){
        long st=bcall(f_transact,(long)sm,4,(long)data,(long)reply,0,0);
        if(st==0){
            long rdz=f_dsize?bcall(f_dsize,(long)reply,0,0,0,0,0):-1;
            unsigned char* rd=f_data?(unsigned char*)bcall(f_data,(long)reply,0,0,0,0,0):0;
            /* readStrongBinder validates against the parcel's object table, which
             * doesn't register the binder inside the rich Service reply. Pull the
             * handle straight out of the flat_binder_object (type @20, flags @24,
             * handle @28) — the kernel already translated it into recovery's
             * handle space — and build the proxy ourselves. */
            unsigned int handle=0;
            if(rd&&rdz>=36 && *(unsigned int*)(rd+20)==0x73682a85u)
                handle=*(unsigned int*)(rd+28);
            if(handle){
                void* hsp[2]={0,0};
                bcall(f_gsp,(long)psp[0],(long)handle,0,0,0,hsp);
                if(hsp[0]) result=(void*)bcall(f_fromplat,(long)hsp,0,0,0,0,0);
            }
        }else{wr("[BRIDGE] transact st=");wrint(st);wr("\n");}
    }
    bcall(f_pdtor,(long)reply,0,0,0,0,0);
    bcall(f_pdtor,(long)data,0,0,0,0,0);
    return result;}

void* AServiceManager_waitForService(const char* n){
    wr("[TWRP WFS] ");if(n)wr(n);wr("\n");
    for(int i=0;i<40;i++){
        void* res=bridge_lookup(n);
        if(res){wr("[TWRP WFS] bridge FOUND\n");return res;}
        nsleep_250ms();
    }
    wr("[TWRP WFS] bridge exhausted -> NULL\n");return 0;}

void* AServiceManager_checkService(const char* n){
    wr("[TWRP CHK] ");if(n)wr(n);wr("\n");
    void* res=bridge_lookup(n);
    wr(res?"[TWRP CHK] bridge FOUND\n":"[TWRP CHK] bridge NULL\n");return res;}

/* ============================================================================
 * Parcel::readStrongBinder(sp*) hook — fix binder extraction from replies.
 *
 * TWRP's libbinder fails to pull a binder out of a reply even when the kernel
 * registered the object (osz>0): keystore2's getSecurityLevel returns a plain
 * [status][flat_binder_object(handle=2)][stability] reply, yet TWRP yields null,
 * so the metadata-decrypt chain dies right after it gets keystore2. The keystore2
 * AIDL client lives in libfscrypttwrp.so and calls the out-param overload
 * Parcel::readStrongBinder(sp<IBinder>*) across a library boundary (via the PLT),
 * so LD_PRELOAD can interpose it.
 *
 * Hook: call the real overload; if it leaves the sp null but a BINDER_TYPE_HANDLE
 * object sits at the current data position, read the kernel-translated handle out
 * of the flat_binder_object and build the proxy with getStrongProxyForHandle —
 * the same trick the SM bridge uses. The overloads return status_t (this=x0,
 * sp*=x1), so a plain C hook suffices (no sret gymnastics). */
static int rsb_recover(void* thisP,void** val){
    static int d=0;
    static void *r_self,*r_gsp,*r_dpos,*r_dsize,*r_data,*r_setpos;
    if(!d){ d=1; if(dlsym){
        r_self  =dlsym((void*)-1L,"_ZN7android12ProcessState4selfEv");
        r_gsp   =dlsym((void*)-1L,"_ZN7android12ProcessState23getStrongProxyForHandleEi");
        r_dpos  =dlsym((void*)-1L,"_ZNK7android6Parcel12dataPositionEv");
        r_dsize =dlsym((void*)-1L,"_ZNK7android6Parcel8dataSizeEv");
        r_data  =dlsym((void*)-1L,"_ZNK7android6Parcel4dataEv");
        r_setpos=dlsym((void*)-1L,"_ZNK7android6Parcel15setDataPositionEm");
    }}
    if(!r_data||!r_dpos||!r_dsize||!r_gsp||!r_self||!r_setpos) return 0;
    long dsz=bcall(r_dsize,(long)thisP,0,0,0,0,0);
    unsigned char* dat=(unsigned char*)bcall(r_data,(long)thisP,0,0,0,0,0);
    if(!dat||dsz<24) return 0;
    /* The real read may have advanced past the object; scan the parcel for a
     * BINDER_TYPE_HANDLE flat object and recover its handle. */
    long mpos=-1;
    for(long off=0; off+24<=dsz; off+=4){
        if(*(unsigned int*)(dat+off)==0x73682a85u){mpos=off;break;}
    }
    if(mpos<0) return 0;
    unsigned int handle=*(unsigned int*)(dat+mpos+8);
    if(!handle) return 0;
    void* psp[2]={0,0}; bcall(r_self,0,0,0,0,0,psp);
    void* hsp[2]={0,0}; bcall(r_gsp,(long)psp[0],(long)handle,0,0,0,hsp);
    if(!hsp[0]) return 0;
    *val=hsp[0];
    bcall(r_setpos,(long)thisP,mpos+28,0,0,0,0);           /* past object+stability */
    wr("[RSB-FIX] recovered handle=");wrint((long)handle);wr("\n");
    return 1;
}
int _ZNK7android6Parcel16readStrongBinderEPNS_2spINS_7IBinderEEE(void* thisP,void** val){
    typedef int(*fn)(void*,void**); static fn r=0;
    if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"_ZNK7android6Parcel16readStrongBinderEPNS_2spINS_7IBinderEEE");
    int st=r?r(thisP,val):-1;
    if(val&&!*val&&rsb_recover(thisP,val))return 0;
    return st;}
int _ZNK7android6Parcel24readNullableStrongBinderEPNS_2spINS_7IBinderEEE(void* thisP,void** val){
    typedef int(*fn)(void*,void**); static fn r=0;
    if(!r&&dlsym)r=(fn)dlsym((void*)-1L,"_ZNK7android6Parcel24readNullableStrongBinderEPNS_2spINS_7IBinderEEE");
    int st=r?r(thisP,val):-1;
    if(val&&!*val&&rsb_recover(thisP,val))return 0;
    return st;}

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
