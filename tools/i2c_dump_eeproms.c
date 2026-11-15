// SPDX-License-Identifier: AGPL-3.0-or-later
/* Dump I2C EEPROMs on PowerPC 32-bit Linux */

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_ioctl 54

#define I2C_SLAVE_FORCE 0x0706

static inline long syscall1(long nr, long a1) {
    register long r0 asm("r0") = nr;
    register long r3 asm("r3") = a1;
    long cr0;
    asm volatile("sc; mfcr %1" : "+r"(r3), "=r"(cr0) : "r"(r0) : "memory");
    if (cr0 & 0x10000000) return -r3;
    return r3;
}

static inline long syscall3(long nr, long a1, long a2, long a3) {
    register long r0 asm("r0") = nr;
    register long r3 asm("r3") = a1;
    register long r4 asm("r4") = a2;
    register long r5 asm("r5") = a3;
    long cr0;
    asm volatile("sc; mfcr %1" : "+r"(r3), "=r"(cr0) : "r"(r0), "r"(r4), "r"(r5) : "memory");
    if (cr0 & 0x10000000) return -r3;
    return r3;
}

static void print_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}

static const char hexmap[] = "0123456789abcdef";

static void print_hex2(unsigned char val) {
    char buf[3];
    buf[0] = hexmap[(val >> 4) & 0xf];
    buf[1] = hexmap[val & 0xf];
    buf[2] = 0;
    print_str(buf);
}

static void dump_device(int fd, int addr) {
    syscall3(SYS_ioctl, fd, I2C_SLAVE_FORCE, addr);

    print_str("\n--- Device 0x");
    print_hex2(addr);
    print_str(" ---\n");

    // Send offset 0
    unsigned char reg = 0;
    syscall3(SYS_write, fd, (long)&reg, 1);

    unsigned char buf[64];
    long n = syscall3(SYS_read, fd, (long)buf, 64);
    if (n <= 0) {
        print_str("Read failed\n");
        return;
    }

    for (int i = 0; i < n; i++) {
        print_hex2(buf[i]);
        print_str(" ");
        if ((i % 16) == 15) {
            print_str(" |");
            for (int j = i - 15; j <= i; j++) {
                char c = buf[j];
                char s[2] = {(c >= 32 && c <= 126) ? c : '.', 0};
                print_str(s);
            }
            print_str("|\n");
        }
    }
}

int main(int argc, char **argv) {
    int fd = syscall3(SYS_open, (long)"/dev/i2c-0", 2 /* O_RDWR */, 0);
    if (fd < 0) {
        print_str("ERR: open /dev/i2c-0\n");
        syscall1(SYS_exit, 1);
    }

    dump_device(fd, 0x50);
    dump_device(fd, 0x51);
    dump_device(fd, 0x52);
    dump_device(fd, 0x53);
    dump_device(fd, 0x69);

    syscall1(SYS_close, fd);
    syscall1(SYS_exit, 0);
    return 0;
}

asm(
    ".section .text\n"
    ".globl _start\n"
    "_start:\n"
    "    lwz 3, 0(1)\n"
    "    addi 4, 1, 4\n"
    "    b main\n"
);
