// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * pci_bar_test.c - Safe userspace PCIe BAR prober for Cisco 3702i
 * Author: Antigravity Pair Programmer
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>

static sigjmp_buf jmp_env;
static volatile sig_atomic_t got_sigbus = 0;

static void sigbus_handler(int sig)
{
    got_sigbus = 1;
    siglongjmp(jmp_env, 1);
}

static uint32_t safe_read32(volatile uint32_t *addr, int *ok)
{
    uint32_t val = 0;
    got_sigbus = 0;
    if (sigsetjmp(jmp_env, 1) == 0) {
        val = *addr;
        *ok = 1;
    } else {
        *ok = 0;
    }
    return val;
}

static void probe_bar(const char *path, const char *bar_name)
{
    printf("\n=== Probing %s (%s) ===\n", bar_name, path);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open failed");
        return;
    }

    size_t size = 64 * 1024;
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return;
    }

    printf("Mapped %s at %p\n", bar_name, map);

    uint32_t offsets[] = {
        0x0000, 0x0004, 0x0008, 0x000c, 0x0010,
        0x0c00, 0x0c04, 0x0c08, 0x0c0c, 0x0c10, 0x0c14, 0x0c18, 0x0c1c, 0x0c20,
        0x0c30, 0x0c34, 0x0c38, 0x0c3c, 0x0c40, 0x0e0e4
    };
    int n_offsets = sizeof(offsets) / sizeof(offsets[0]);

    for (int i = 0; i < n_offsets; i++) {
        uint32_t off = offsets[i];
        if (off >= size) continue;

        volatile uint32_t *p = (volatile uint32_t *)((char *)map + off);
        int ok = 0;
        uint32_t val = safe_read32(p, &ok);
        if (ok) {
            printf("  [%s + 0x%04x] = 0x%08x\n", bar_name, off, val);
        } else {
            printf("  [%s + 0x%04x] = FAULT (SIGBUS / Target Abort)\n", bar_name, off);
        }
    }

    munmap(map, size);
    close(fd);
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigbus_handler;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    probe_bar("/sys/bus/pci/devices/a000:03:00.0/resource0", "BAR0 (0x80000000)");
    probe_bar("/sys/bus/pci/devices/a000:03:00.0/resource2", "BAR2 (0x80010000)");

    return 0;
}
