/* Startup: set the stack, clear .bss, call main, pass its return value to exit. */
    .section .text.start
    .globl _start
_start:
    li    sp, 0x000FFFF0
    la    t0, __bss_start
    la    t1, __bss_end
1:  bgeu  t0, t1, 2f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     1b
2:  call  main
    li    a7, 93          /* exit(a0) */
    ecall
3:  j     3b
