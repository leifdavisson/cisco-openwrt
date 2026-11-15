// SPDX-License-Identifier: AGPL-3.0-or-later
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define I2C_SLAVE_FORCE 0x0706

int main(void) {
    int fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0 || ioctl(fd, I2C_SLAVE_FORCE, 0x69) < 0) return 1;
    for (int r = 0x08; r <= 0x57; r++) {
        unsigned char reg = r, val = 0xff;
        if (write(fd, &reg, 1) != 1) printf("wr err %02x\n", r);
        if (read(fd, &val, 1) != 1) printf("rd err %02x\n", r);
        printf("reg 0x%02x = 0x%02x\n", r, val);
    }
    close(fd);
    return 0;
}
