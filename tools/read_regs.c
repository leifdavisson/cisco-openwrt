// SPDX-License-Identifier: AGPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define CCSR_BASE 0xe0000000
#define CCSR_SIZE 0x200000 // 2MB

int main() {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    uint8_t *ccsr = mmap(NULL, CCSR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CCSR_BASE);
    if (ccsr == MAP_FAILED) {
        perror("mmap CCSR");
        close(fd);
        return 1;
    }

    // SVR is at GUTS offset 0xe00a4
    uint32_t svr = *(volatile uint32_t *)(ccsr + 0xe00a4);
    uint32_t pvr;
    asm volatile("mfpvr %0" : "=r"(pvr));

    printf("=== CPU & SOC IDENTIFICATION ===\n");
    printf("PVR: 0x%08x\n", pvr);
    printf("SVR: 0x%08x\n", svr);

    // Decode SVR
    uint32_t dev = (svr >> 16) & 0xffff;
    printf("SVR Device ID: 0x%04x (MajRev:%d MinRev:%d)\n", dev, (svr >> 4) & 0xf, svr & 0xf);

    // Check GUTS PORDEVSR (0xe0000)
    uint32_t pordevsr = *(volatile uint32_t *)(ccsr + 0xe0000);
    printf("PORDEVSR: 0x%08x\n", pordevsr);

    printf("\n=== PROBING ETSEC vs FMAN REGISTERS ===\n");
    // eTSEC MDIO at 0x24520
    uint32_t miimcfg_etsec = *(volatile uint32_t *)(ccsr + 0x24520);
    uint32_t miimind_etsec = *(volatile uint32_t *)(ccsr + 0x24534);
    printf("eTSEC 0x24520 (MIIMCFG): 0x%08x, 0x24534 (MIIMIND): 0x%08x\n", miimcfg_etsec, miimind_etsec);

    // FMan 1 at 0x1e0000
    uint32_t fman_id = *(volatile uint32_t *)(ccsr + 0x1e0000);
    uint32_t fman_miimcfg = *(volatile uint32_t *)(ccsr + 0x1e1120);
    uint32_t fman_miimind = *(volatile uint32_t *)(ccsr + 0x1e1134);
    printf("FMan  0x1e0000 (FMan ID): 0x%08x\n", fman_id);
    printf("FMan  0x1e1120 (MIIMCFG): 0x%08x, 0x1e1134 (MIIMIND): 0x%08x\n", fman_miimcfg, fman_miimind);

    // Try reading PHY via FMan MDIO if present
    volatile uint32_t *fm_cfg = (volatile uint32_t *)(ccsr + 0x1e1120);
    volatile uint32_t *fm_com = (volatile uint32_t *)(ccsr + 0x1e1124);
    volatile uint32_t *fm_add = (volatile uint32_t *)(ccsr + 0x1e1128);
    volatile uint32_t *fm_stat = (volatile uint32_t *)(ccsr + 0x1e1130);
    volatile uint32_t *fm_ind = (volatile uint32_t *)(ccsr + 0x1e1134);

    printf("\n=== PROBING FMAN MDIO PHY REGISTERS (Addr 0) ===\n");
    for (int reg = 0; reg < 4; reg++) {
        *fm_add = (0 << 8) | reg;
        *fm_com = 0;
        *fm_com = 1;
        usleep(100);
        uint32_t stat = *fm_stat;
        uint32_t ind = *fm_ind;
        printf("FMAN PHY reg %d: val=0x%04x (ind=0x%08x)\n", reg, stat & 0xffff, ind);
    }

    munmap(ccsr, CCSR_SIZE);
    close(fd);
    return 0;
}
