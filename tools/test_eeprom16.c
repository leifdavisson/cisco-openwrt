
// SPDX-License-Identifier: AGPL-3.0-or-later
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
    int len = 0; while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}
static const char hexmap[] = "0123456789abcdef";
static void print_hex2(unsigned char val) {
    char buf[3] = {hexmap[(val >> 4) & 0xf], hexmap[val & 0xf], 0};
    print_str(buf);
}
static void test_dev(int fd, int addr) {
    syscall3(SYS_ioctl, fd, I2C_SLAVE_FORCE, addr);
    print_str("\nTesting 2-byte addr on 0x");
    print_hex2(addr);
    print_str(":\n");
    unsigned char reg16[2] = {0, 0};
    long w = syscall3(SYS_write, fd, (long)reg16, 2);
    print_str("write 2B returned: ");
    print_hex2((unsigned char)w);
    print_str("\n");
    unsigned char buf[32];
    long r = syscall3(SYS_read, fd, (long)buf, 32);
    print_str("read returned: ");
    print_hex2((unsigned char)r);
    print_str("\nData: ");
    for (int i = 0; i < (r > 0 ? r : 0); i++) {
        print_hex2(buf[i]); print_str(" ");
    }
    print_str("\n");
}
int main() {
    int fd = syscall3(SYS_open, (long)"/dev/i2c-0", 2, 0);
    test_dev(fd, 0x51);
    test_dev(fd, 0x52);
    test_dev(fd, 0x53);
    syscall1(SYS_close, fd);
    syscall1(SYS_exit, 0);
    return 0;
}
asm(".section .text\n.globl _start\n_start:\nlwz 3, 0(1)\naddi 4, 1, 4\nb main\n");
