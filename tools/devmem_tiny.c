// SPDX-License-Identifier: AGPL-3.0-or-later
/* Minimal standalone devmem tool for PowerPC 32-bit Linux */

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_mmap2 192

static inline long syscall1(long nr, long a1) {
    register long r0 asm("r0") = nr;
    register long r3 asm("r3") = a1;
    asm volatile("sc" : "+r"(r3) : "r"(r0) : "memory", "cr0");
    return r3;
}

static inline long syscall3(long nr, long a1, long a2, long a3) {
    register long r0 asm("r0") = nr;
    register long r3 asm("r3") = a1;
    register long r4 asm("r4") = a2;
    register long r5 asm("r5") = a3;
    asm volatile("sc" : "+r"(r3) : "r"(r0), "r"(r4), "r"(r5) : "memory", "cr0");
    return r3;
}

static inline void *sys_mmap2(void *addr, unsigned long len, unsigned long prot, unsigned long flags, int fd, unsigned long pgoff) {
    register long r0 asm("r0") = SYS_mmap2;
    register long r3 asm("r3") = (long)addr;
    register long r4 asm("r4") = len;
    register long r5 asm("r5") = prot;
    register long r6 asm("r6") = flags;
    register long r7 asm("r7") = fd;
    register long r8 asm("r8") = pgoff;
    long cr0;
    asm volatile("sc; mfcr %1" : "+r"(r3), "=r"(cr0) : "r"(r0), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r8) : "memory");
    if (cr0 & 0x10000000) {
        return (void *)-r3;
    }
    return (void *)r3;
}

static void print_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}

static void print_hex(unsigned int val) {
    char buf[12];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        int nib = (val >> (i * 4)) & 0xf;
        buf[9 - i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[10] = '\n';
    buf[11] = 0;
    print_str(buf);
}

static unsigned int parse_hex(const char *s) {
    unsigned int val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
    }
    return val;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_str("Usage: devmem <phys_addr> [val]\n");
        syscall1(SYS_exit, 1);
    }

    unsigned int target = parse_hex(argv[1]);
    unsigned int page_base = target & ~0xfff;
    unsigned int offset = target & 0xfff;
    int is_write = (argc >= 3);
    unsigned int write_val = is_write ? parse_hex(argv[2]) : 0;

    int fd = syscall3(SYS_open, (long)"/dev/mem", 2 /* O_RDWR */, 0);
    if (fd < 0) {
        print_str("ERR: open /dev/mem\n");
        syscall1(SYS_exit, 1);
    }

    void *map = sys_mmap2(0, 0x1000, 3 /* PROT_READ|WRITE */, 1 /* MAP_SHARED */, fd, page_base >> 12);
    if ((long)map < 0 && (long)map > -4096) {
        print_str("ERR: mmap2 errno=");
        print_hex((unsigned int)-(long)map);
        syscall1(SYS_exit, 1);
    }

    volatile unsigned int *reg = (volatile unsigned int *)((unsigned long)map + offset);

    if (is_write) {
        *reg = write_val;
    }

    unsigned int read_val = *reg;
    print_hex(read_val);

    syscall1(SYS_close, fd);
    syscall1(SYS_exit, 0);
    return 0;
}

asm(
    ".section .text\n"
    ".globl _start\n"
    "_start:\n"
    "    lwz 3, 0(1)\n"      /* r3 = argc */
    "    addi 4, 1, 4\n"     /* r4 = argv */
    "    b main\n"
);
