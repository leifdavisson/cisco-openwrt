// SPDX-License-Identifier: AGPL-3.0-or-later
/* Freestanding PowerPC 32-bit register probe */

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
        return (void *)-1;
    }
    return (void *)r3;
}

static void print_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}

static void print_hex(unsigned int val) {
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        int nib = (val >> (i * 4)) & 0xf;
        buf[9 - i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[10] = 0;
    print_str(buf);
}

void _start() {
    int fd = syscall3(SYS_open, (long)"/dev/mem", 2 /* O_RDWR */, 0);
    if (fd < 0) {
        print_str("ERR: open /dev/mem failed\n");
        syscall1(SYS_exit, 1);
    }

    void *map = sys_mmap2(0, 0x200000, 3 /* PROT_READ|WRITE */, 1 /* MAP_SHARED */, fd, 0xe0000);
    if (map == (void *)-1) {
        print_str("ERR: mmap2 failed\n");
        syscall1(SYS_exit, 1);
    }

    unsigned long ccsr = (unsigned long)map;

    unsigned int pvr;
    asm volatile("mfpvr %0" : "=r"(pvr));

    unsigned int *p_guts = (unsigned int *)(ccsr + 0xe0000);
    unsigned int pordevsr = p_guts[0];      // 0xe0000
    unsigned int svr = p_guts[0xa4 / 4];    // 0xe00a4

    print_str("\n--- CPU & SOC INFO ---\n");
    print_str("PVR: "); print_hex(pvr); print_str("\n");
    print_str("SVR: "); print_hex(svr); print_str("\n");
    print_str("PORDEVSR: "); print_hex(pordevsr); print_str("\n");

    print_str("\n--- ETSEC MIIM (0x24520) ---\n");
    unsigned int *p_etsec = (unsigned int *)(ccsr + 0x24520);
    print_str("MIIMCFG: "); print_hex(p_etsec[0]); print_str("\n");
    print_str("MIIMIND: "); print_hex(p_etsec[5]); print_str("\n");

    print_str("\n--- FMAN REGISTERS (0x1e0000) ---\n");
    unsigned int *p_fman = (unsigned int *)(ccsr + 0x1e0000);
    print_str("FMAN_ID: "); print_hex(p_fman[0]); print_str("\n");

    unsigned int *p_fman_miim = (unsigned int *)(ccsr + 0x1e1120);
    print_str("FMAN_MIIMCFG: "); print_hex(p_fman_miim[0]); print_str("\n");
    print_str("FMAN_MIIMIND: "); print_hex(p_fman_miim[5]); print_str("\n");

    // Test FMan MIIM read at PHY addr 0
    print_str("\n--- FMAN MIIM READ PHY 0 ---\n");
    volatile unsigned int *fm_add = (volatile unsigned int *)(ccsr + 0x1e1128);
    volatile unsigned int *fm_com = (volatile unsigned int *)(ccsr + 0x1e1124);
    volatile unsigned int *fm_stat = (volatile unsigned int *)(ccsr + 0x1e1130);
    volatile unsigned int *fm_ind = (volatile unsigned int *)(ccsr + 0x1e1134);

    for (int r = 0; r < 5; r++) {
        *fm_add = (0 << 8) | r;
        *fm_com = 0;
        *fm_com = 1;
        for (volatile int d = 0; d < 20000; d++);
        print_str("Reg "); print_hex(r);
        print_str(": val="); print_hex(*fm_stat & 0xffff);
        print_str(" ind="); print_hex(*fm_ind);
        print_str("\n");
    }

    // Also scan all PHY addresses 0..31 on FMan MDIO reg 2 and 3 (PHY ID 1 and 2)
    print_str("\n--- SCANNING FMAN MDIO PHY ADDRS 0..7 ---\n");
    for (int phy = 0; phy < 8; phy++) {
        *fm_add = (phy << 8) | 2; // PHYID 1
        *fm_com = 0;
        *fm_com = 1;
        for (volatile int d = 0; d < 20000; d++);
        unsigned int id1 = *fm_stat & 0xffff;
        *fm_add = (phy << 8) | 3; // PHYID 2
        *fm_com = 0;
        *fm_com = 1;
        for (volatile int d = 0; d < 20000; d++);
        unsigned int id2 = *fm_stat & 0xffff;

        if (id1 != 0xffff && id1 != 0) {
            print_str("Found PHY at "); print_hex(phy);
            print_str(" ID: "); print_hex((id1 << 16) | id2);
            print_str("\n");
        }
    }

    print_str("\n--- SCANNING ETSEC MIIM PHY ADDRS 0..7 ---\n");
    volatile unsigned int *et_add = (volatile unsigned int *)(ccsr + 0x24528);
    volatile unsigned int *et_com = (volatile unsigned int *)(ccsr + 0x24524);
    volatile unsigned int *et_stat = (volatile unsigned int *)(ccsr + 0x24530);

    for (int phy = 0; phy < 8; phy++) {
        *et_add = (phy << 8) | 2;
        *et_com = 0;
        *et_com = 1;
        for (volatile int d = 0; d < 20000; d++);
        unsigned int id1 = *et_stat & 0xffff;
        *et_add = (phy << 8) | 3;
        *et_com = 0;
        *et_com = 1;
        for (volatile int d = 0; d < 20000; d++);
        unsigned int id2 = *et_stat & 0xffff;

        if (id1 != 0xffff && id1 != 0) {
            print_str("Found eTSEC PHY at "); print_hex(phy);
            print_str(" ID: "); print_hex((id1 << 16) | id2);
            print_str("\n");
        }
    }

    syscall1(SYS_close, fd);
    syscall1(SYS_exit, 0);
}
