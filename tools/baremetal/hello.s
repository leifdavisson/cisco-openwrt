/*
 * hello.s - Minimal PowerPC bare-metal payload for Cisco Aironet 3702i.
 * Entry point: 0x02003000
 * License: GNU AGPLv3
 *
 * Hardware: Cisco Aironet 3702i (Montserrat) - Freescale P1020 e500v2
 * CCSR Register Base in ROMMON: 0xE0000000
 * UART0 THR: 0xE0004500
 * UART0 LSR: 0xE0004505
 */

.text
.globl _start

_start:
    /* Load CCSR UART base (0xE0004500) into r4 */
    lis     %r4, 0xe000
    ori     %r4, %r4, 0x4500     /* r4 = 0xE0004500 (UART0 Base) */

    /* Get address of banner string */
    bl      1f
banner:
    .asciz  "\r\n\r\n======================================================================\r\n[+] OPENWRT BARE-METAL BOOTSTRAP EXECUTED SUCCESSFULLY!\r\n[+] Target Hardware: Cisco Aironet 3702i (ap3g2 / Montserrat)\r\n[+] CPU: Freescale QorIQ P1020 e500v2 PowerPC\r\n[+] Execution Base: 0x02003000 (RAM)\r\n[+] CCSRBAR: 0xE0000000 (UART0 @ 0xE0004500, 9600 8N1)\r\n[+] ROMMON Signature Bypass: 100% SUCCESSFUL!\r\n======================================================================\r\n\r\n"
    .balign 4

1:
    mflr    %r31                 /* r31 = pointer to string */

print_loop:
    lbz     %r3, 0(%r31)
    cmpwi   %r3, 0
    beq     spin_heartbeat

    /* Wait for UART LSR THRE (bit 5, 0x20) and TEMT (bit 6, 0x40) */
wait_tx:
    lbz     %r5, 5(%r4)          /* 0xE0004505 = UART LSR */
    andi.   %r6, %r5, 0x20       /* THRE */
    beq     wait_tx

    stb     %r3, 0(%r4)          /* 0xE0004500 = UART THR */
    addi    %r31, %r31, 1
    b       print_loop

spin_heartbeat:
    /* Delay loop */
    lis     %r10, 0x0080
delay_loop:
    addic.  %r10, %r10, -1
    bne     delay_loop

    /* Send a heartbeat dot '.' */
    li      %r3, 0x2e            /* '.' */
wait_dot:
    lbz     %r5, 5(%r4)
    andi.   %r6, %r5, 0x20
    beq     wait_dot
    stb     %r3, 0(%r4)

    b       spin_heartbeat
