/* bind_mount.c — bind-mounts argv[1] onto argv[2] (MS_BIND).
 *
 * Used to splice our private binderfs node over the stock one:
 *     bind_mount /dev/newbfs/binder /dev/binderfs/binder
 * A tiny static helper because TWRP's toybox `mount` lacks --bind here.
 *
 * Build: aarch64-linux-gnu-gcc -static -O0 -o bind_mount bind_mount.c
 */
#include <sys/mount.h>
int main(int argc, char* argv[]) {
    if (argc < 3) return 1;
    return mount(argv[1], argv[2], 0, MS_BIND, 0) == 0 ? 0 : 1;
}
