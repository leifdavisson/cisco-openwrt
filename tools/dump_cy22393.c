// SPDX-License-Identifier: AGPL-3.0-or-later
/* Dump CY22393 registers via /dev/i2c-0 */

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_ioctl 54

#define I2C_RDWR 0x0707
#define I2C_M_RD 0x0001

struct i2c_msg {
    unsigned short addr;
    unsigned short flags;
    short len;
    unsigned char *buf;
};

struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    unsigned int nmsgs;
};

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

int main() {
    int fd = syscall3(SYS_open, (long)"/dev/i2c-0", 2, 0);
    if (fd < 0) {
        print_str("ERR: cannot open /dev/i2c-0\n");
        syscall1(SYS_exit, 1);
    }

    print_str("Dumping CY22393 (0x69) registers (0x08 - 0x57):\n");
    print_str("     00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f\n");

    for (int row = 0; row < 0x60; row += 16) {
        print_hex2(row);
        print_str(": ");
        for (int col = 0; col < 16; col++) {
            unsigned char reg = row + col;
            unsigned char val = 0;

            struct i2c_msg msgs[2];
            msgs[0].addr = 0x69;
            msgs[0].flags = 0;
            msgs[0].len = 1;
            msgs[0].buf = &reg;

            msgs[1].addr = 0x69;
            msgs[1].flags = I2C_M_RD;
            msgs[1].len = 1;
            msgs[1].buf = &val;

            struct i2c_rdwr_ioctl_data rdwr;
            rdwr.msgs = msgs;
            rdwr.nmsgs = 2;

            long ret = syscall3(SYS_ioctl, fd, I2C_RDWR, (long)&rdwr);
            if (ret >= 0) {
                print_hex2(val);
                print_str(" ");
            } else {
                print_str("-- ");
            }
        }
        print_str("\n");
    }

    syscall1(SYS_close, fd);
    syscall1(SYS_exit, 0);
    return 0;
}

asm(
    ".section .text\n"
    ".globl _start\n"
    "_start:\n"
    "    lwz 3, 0(1)\n"
    "    la 4, 4(1)\n"
    "    b main\n"
);
