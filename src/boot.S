/* Ktuyome boot stub: Multiboot 1 header (requests a 1280x720x32 linear framebuffer),
   flat GDT, stack, x87 init, and the interrupt stubs. */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000007
.set MB_CHECK, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECK
.long 0, 0, 0, 0, 0
.long 0          /* linear graphics mode */
.long 1280
.long 720
.long 32

.section .bss
.align 16
stack_bottom: .skip 65536
stack_top:

.section .text
.global _start
_start:
    cli
    mov $stack_top, %esp
    push %ebx
    push %eax
    lgdt gdt_desc
    ljmp $0x08, $1f
1:  mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    fninit
    call kmain
2:  cli
    hlt
    jmp 2b

.section .rodata
.align 8
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt_desc:
    .word 23
    .long gdt

.altmacro
.macro MKISR n
.section .text
isr\n:
    .if ((\n==8) || (\n==17) || (\n==30) || ((\n>=10) && (\n<=14)))
    .else
    pushl $0
    .endif
    pushl $\n
    jmp isr_common
.section .rodata
    .long isr\n
.endm

.section .rodata
.align 4
.global isr_table
isr_table:
.set i, 0
.rept 48
    MKISR %i
    .set i, i+1
.endr

.section .text
isr_common:
    pusha
    cld
    mov %esp, %eax
    push %eax
    call isr_dispatch
    add $4, %esp
    popa
    add $8, %esp
    iret
