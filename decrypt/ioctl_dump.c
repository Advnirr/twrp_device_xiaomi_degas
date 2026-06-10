/* ===========================================================================
 * ioctl_dump.c  —  LD_PRELOAD shim that hex-dumps binder BINDER_WRITE_READ
 *                  ioctl traffic (the raw parcels) for a single process.
 * ===========================================================================
 *
 * Purpose: diagnose why TWRP's /tw_libs/libbinder.so fails to look up a service
 * that the stock system libbinder finds fine. Run the stock `service` tool once
 * with the SYSTEM libs, and once with the TWRP libs forced in, dumping the exact
 * checkService transaction bytes from each so they can be diffed:
 *
 *   LD_PRELOAD=/tmp/ioctl_dump.so LD_LIBRARY_PATH=/mnt_sys/system/lib64:/vendor/lib64 \
 *       service check 'android.system.keystore2.IKeystoreService/default'   # works
 *   LD_PRELOAD=/tmp/ioctl_dump.so LD_LIBRARY_PATH=/tw_libs \
 *       service check 'android.system.keystore2.IKeystoreService/default'   # TWRP libs
 *
 * BINDER_WRITE_READ = _IOWR('b',1,binder_write_read) = 0xC0306201 (aarch64).
 * struct binder_write_read (64-bit):
 *   off 0  u64 write_size
 *   off 8  u64 write_consumed
 *   off 16 u64 write_buffer (ptr)
 *   off 24 u64 read_size
 *   off 32 u64 read_consumed
 *   off 40 u64 read_buffer (ptr)
 *
 * Build:  aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -O0 -std=c11 \
 *             -o ioctl_dump.so ioctl_dump.c
 * ===========================================================================
 */
#include <stddef.h>

static void wr(const char* s){
    if(!s||!*s)return;const char* e=s;while(*e)e++;
    __asm__ volatile("mov x8,#64\nmov x0,#2\nmov x1,%0\nsub x2,%1,%0\nsvc #0\n"
        ::"r"(s),"r"(e):"x0","x1","x2","x8","memory");}

static void wrn(unsigned long v){
    char b[21];int i=20;b[i]='\0';
    if(!v)b[--i]='0';else while(v){b[--i]=(char)('0'+v%10);v/=10;}
    wr(b+i);}

/* hex-dump `n` bytes at p, 32 bytes per line, with a leading tag. */
static void hexdump(const char* tag,const unsigned char* p,unsigned long n){
    if(!p||!n){wr(tag);wr(" (empty)\n");return;}
    const char* H="0123456789abcdef";
    char line[3*32+1];
    unsigned long off=0;
    while(off<n){
        unsigned long m=n-off; if(m>32)m=32;
        int j=0;
        for(unsigned long k=0;k<m;k++){
            unsigned char c=p[off+k];
            line[j++]=H[c>>4];line[j++]=H[c&0xf];line[j++]=' ';}
        line[j]='\0';
        wr(tag);wr(" +");wrn(off);wr(": ");wr(line);wr("\n");
        off+=m;}
}

extern void* dlsym(void*,const char*) __attribute__((weak));

__attribute__((constructor)) static void init(void){wr("=== ioctl_dump loaded ===\n");}

int ioctl(int fd,unsigned long req,long arg){
    typedef int(*fn)(int,unsigned long,long);
    static fn real=0;
    if(!real&&dlsym)real=(fn)dlsym((void*)-1L,"ioctl");
    if(!real)return -1;

    if(req==0xC0306201ul && arg){
        unsigned long* b=(unsigned long*)arg;
        unsigned long wsize=b[0];
        unsigned char* wbuf=(unsigned char*)b[2];
        /* If the first command is BC_TRANSACTION (0x40406300) to handle 0, the
         * binder_transaction_data follows; the real Parcel payload lives at
         * btd.data.ptr.buffer (off 48 in the btd), length btd.data_size (off 32).
         * Follow that pointer so we see the actual interface token + name. */
        if(wsize>=4+64 && *(unsigned int*)wbuf==0x40406300u){
            unsigned char* btd=wbuf+4;
            unsigned long handle=*(unsigned long*)(btd+0);
            unsigned int code=*(unsigned int*)(btd+16);
            unsigned int flags=*(unsigned int*)(btd+20);
            unsigned long dsize=*(unsigned long*)(btd+32);
            unsigned char* dbuf=(unsigned char*)*(unsigned long*)(btd+48);
            wr("[IOD] TX handle=");wrn(handle);
            wr(" code=");wrn(code);
            wr(" flags=");wrn(flags);
            wr(" data_size=");wrn(dsize);wr("\n");
            if(dbuf&&dsize&&dsize<4096)hexdump("[IOD TXDATA]",dbuf,dsize);
        }
        int r=real(fd,req,arg);
        /* Reply parcel: scan read buffer for BR_REPLY (0x80407202); its btd's
         * data.ptr.buffer/data_size hold the reply Parcel (status + binder). */
        unsigned long rcons=b[4];
        unsigned char* rbuf=(unsigned char*)b[5];
        unsigned long off=0;
        while(off+4<=rcons){
            unsigned int cmd=*(unsigned int*)(rbuf+off);
            if(cmd==0x80407203u && off+4+64<=rcons){ /* BR_REPLY = _IOR('r',3,btd) */
                unsigned char* btd=rbuf+off+4;
                unsigned long dsize=*(unsigned long*)(btd+32);
                unsigned char* dbuf=(unsigned char*)*(unsigned long*)(btd+48);
                wr("[IOD] REPLY data_size=");wrn(dsize);wr("\n");
                if(dbuf&&dsize&&dsize<4096)hexdump("[IOD RPDATA]",dbuf,dsize);
                break;
            }
            off+=4; /* coarse scan */
        }
        return r;
    }
    return real(fd,req,arg);
}
