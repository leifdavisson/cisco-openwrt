// SPDX-License-Identifier: AGPL-3.0-or-later
/* Test I2C_RDWR with Repeated START */

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

static void do_test(int fd, int addr) {
    print_str("\nTesting I2C_RDWR on 0x");
    print_hex2(addr);
    print_str("...\n");

    unsigned char addr_buf[2] = {0, 0};
    unsigned char read_buf[64];
    for (int i = 0; i < 64; i++) read_buf[i] = 0xcc;

    struct i2c_msg msgs[2];
    msgs[0].addr = addr;
    msgs[0].flags = 0;
    msgs[0].len = 2;
    msgs[0].buf = addr_buf;

    msgs[1].addr = addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = 64;
    msgs[1].buf = read_buf;

    struct i2c_rdwr_ioctl_data rdwr;
    rdwr.msgs = msgs;
    rdwr.nmsgs = 2;

    long ret = syscall3(SYS_ioctl, fd, I2C_RDWR, (long)&rdwr);
    print_str("ioctl I2C_RDWR returned: ");
    print_hex2((unsigned char)ret);
    print_str("\n");

    if (ret >= 0) {
        print_str("Read data:\n");
        for (int i = 0; i < 64; i++) {
            print_hex2(read_buf[i]);
            print_str(" ");
            if ((i % 16) == 15) {
                print_str(" |");
                for (int j = i - 15; j <= i; j++) {
                    char c = read_buf[j];
                    char s[2] = {(c >= 32 && c <= 126) ? c : '.', 0};
                    print_str(s);
                }
                print_str("|\n");
            }
        }
    }
}

int main() {
    int fd = syscall3(SYS_open, (long)"/dev/i2c-0", 2, 0);
    if (fd < 0) {
        print_str("ERR: open\n");
        syscall1(SYS_exit, 1);
    }

    do_test(fd, 0x50);
    do_test(fd, 0x51);
    do_test(fd, 0x52);

    syscall1(SYS_close, fd);
    syscall1(SYS_exit, 0);
    return 0;
}

asm(".section .text\n.globl _start\n_start:\nlwz 3, 0(1)\naddi 4, 1, 4\nb main\n");
