#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define DISP_MEM_REQUEST 0x2c0
#define DISP_MEM_RELEASE 0x2c1
#define DISP_MEM_GETADR  0x2c2

int main(int argc, char **argv)
{
    int sel = argc > 1 ? atoi(argv[1]) : 9;
    size_t size = 720u * 1280u * 4u;
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { perror("disp"); return 1; }
    unsigned long a[4] = { (unsigned long)sel, (unsigned long)size, 0, 0 };
    printf("req=%d\n", ioctl(fd, DISP_MEM_REQUEST, a));
    a[0] = (unsigned long)sel;
    long raw = ioctl(fd, DISP_MEM_GETADR, a);
    uint32_t p = (uint32_t)(int)raw;
    printf("raw=%ld hexraw=0x%lx masked=0x%x\n", raw, (unsigned long)raw, p);

    void *m = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    printf("disp off0: %p (%s)\n", m, m == MAP_FAILED ? strerror(errno) : "ok");
    if (m != MAP_FAILED) munmap(m, 4096);
    m = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, (off_t)(p & ~4095u));
    printf("disp off=0x%x: %p (%s)\n", p, m, m == MAP_FAILED ? strerror(errno) : "ok");
    if (m != MAP_FAILED) munmap(m, size);
    m = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, (off_t)(p >> 12));
    printf("disp pgoff=0x%x: %p (%s)\n", p >> 12, m, m == MAP_FAILED ? strerror(errno) : "ok");
    if (m != MAP_FAILED) munmap(m, size);

    int mf = open("/dev/mem", O_RDWR);
    printf("open mem=%d\n", mf);
    if (mf >= 0) {
        m = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, mf, (off_t)(p & ~4095u));
        printf("mem off=0x%x: %p (%s)\n", p, m, m == MAP_FAILED ? strerror(errno) : "ok");
        if (m != MAP_FAILED) munmap(m, size);
    }
    a[0] = (unsigned long)sel;
    ioctl(fd, DISP_MEM_RELEASE, a);
    close(fd);
    return 0;
}
