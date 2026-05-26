#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stddef.h>

/* 
 * TCMalloc assumes a 48-bit Virtual Address (VA) space. 
 * Many ARM64 Android kernels (and chroots running on them) are limited to 39 bits.
 * This interposer intercepts mmap calls and clears the hint address if it 
 * exceeds the 39-bit boundary, allowing the kernel to pick a safe address.
 */

#define MAX_VA_BITS 39
#define VA_BOUNDARY (1ULL << MAX_VA_BITS)

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static void* (*real_mmap)(void*, size_t, int, int, int, off_t) = NULL;
    if (!real_mmap) {
        real_mmap = dlsym(RTLD_NEXT, "mmap");
    }

    if ((uintptr_t)addr >= VA_BOUNDARY) {
        addr = NULL;
    }

    return real_mmap(addr, length, prot, flags, fd, offset);
}
