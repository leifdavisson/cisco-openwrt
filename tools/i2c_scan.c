// SPDX-License-Identifier: AGPL-3.0-or-later
/* Standalone I2C scanner for PowerPC 32-bit Linux */

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_ioctl 54

#define I2C_SLAVE 0x0703
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

static void scan_bus(const char *devpath) {
    print_str("\n--- Scanning ");
    print_str(devpath);
    print_str(" ---\n");

    int fd = syscall3(SYS_open, (long)devpath, 2 /* O_RDWR */, 0);
    if (fd < 0) {
        print_str("ERR: cannot open ");
        print_str(devpath);
        print_str("\n");
        return;
    }

    int found = 0;
    for (int addr = 0x03; addr <= 0x77; addr++) {
        long ret = syscall3(SYS_ioctl, fd, I2C_SLAVE_FORCE, addr);
        if (ret < 0) continue;

        unsigned char buf = 0;
        long n = syscall3(SYS_read, fd, (long)&buf, 1);
        if (n == 1) {
            print_str("Found device at 0x");
            print_hex2(addr);
            print_str(" (byte=0x");
            print_hex2(buf);
            print_str(")\n");
            found++;
        }
    }

    if (!found) {
        print_str("No responding devices on ");
        print_str(devpath);
        print_str("\n");
    }

    syscall1(SYS_close, fd);
}

int main(int argc, char **argv) {
    scan_bus("/dev/i2c-0");
    scan_bus("/dev/i2c-1");
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
