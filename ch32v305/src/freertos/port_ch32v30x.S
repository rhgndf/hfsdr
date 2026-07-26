/*
 * CH32V30x exception/interrupt bridges for the upstream FreeRTOS RISC-V
 * context port.  Task yields use the synchronous machine-mode ecall exception;
 * ISR-requested switches use the low-priority PFIC software interrupt.  Task
 * stack initialization, first-task startup, and context frame save/restore
 * remain owned by FreeRTOS-Kernel's portASM.S.
 */

#include "portContext.h"

.extern vTaskSwitchContext

/*
 * A normal C interrupt body is called through one of these wrappers.  QingKe
 * HWSTK preserves the caller-saved integer registers and interrupt return
 * state.  The C ABI preserves the callee-saved integer/FPU registers.  Save
 * the remaining caller-saved FPU state here.
 *
 * The 96-byte frame is 16-byte aligned and contains:
 *   0..76  f0-f7, f10-f17, f28-f31
 *      80  fcsr
 *      84  interrupted SP when switching from a task stack, otherwise zero
 *   88..95 alignment padding
 *
 * Test the actual SP against the IRQ-stack bounds instead of relying on PFIC
 * nesting state.  This also handles a higher-priority interrupt arriving at
 * any instruction in another wrapper's stack-switch or restore sequence.
 * No SP-relative store is performed until the stack choice is complete.
 */
.macro portIRQ_WRAPPER vector, body
.extern \body
.global \vector
.type \vector, @function
.section .text.\vector, "ax", @progbits
.p2align 2
\vector:
    la t0, __freertos_irq_stack_bottom
    la t1, __freertos_irq_stack_top
    mv t2, sp
    bltu sp, t0, .Lirq_use_top_\@
    bgeu sp, t1, .Lirq_use_top_\@
    li t2, 0
    j .Lirq_frame_\@

.Lirq_use_top_\@:
    mv sp, t1

.Lirq_frame_\@:
    addi sp, sp, -96
    sw t2, 84(sp)

    fsw f0,   0(sp)
    fsw f1,   4(sp)
    fsw f2,   8(sp)
    fsw f3,  12(sp)
    fsw f4,  16(sp)
    fsw f5,  20(sp)
    fsw f6,  24(sp)
    fsw f7,  28(sp)
    fsw f10, 32(sp)
    fsw f11, 36(sp)
    fsw f12, 40(sp)
    fsw f13, 44(sp)
    fsw f14, 48(sp)
    fsw f15, 52(sp)
    fsw f16, 56(sp)
    fsw f17, 60(sp)
    fsw f28, 64(sp)
    fsw f29, 68(sp)
    fsw f30, 72(sp)
    fsw f31, 76(sp)
    csrr t0, fcsr
    sw t0, 80(sp)

    call \body

    flw f0,   0(sp)
    flw f1,   4(sp)
    flw f2,   8(sp)
    flw f3,  12(sp)
    flw f4,  16(sp)
    flw f5,  20(sp)
    flw f6,  24(sp)
    flw f7,  28(sp)
    flw f10, 32(sp)
    flw f11, 36(sp)
    flw f12, 40(sp)
    flw f13, 44(sp)
    flw f14, 48(sp)
    flw f15, 52(sp)
    flw f16, 56(sp)
    flw f17, 60(sp)
    flw f28, 64(sp)
    flw f29, 68(sp)
    flw f30, 72(sp)
    flw f31, 76(sp)
    lw t0, 80(sp)
    csrw fcsr, t0

    lw t2, 84(sp)
    addi sp, sp, 96
    beqz t2, .Lirq_return_\@
    mv sp, t2

.Lirq_return_\@:
    mret
.size \vector, .-\vector
.endm

portIRQ_WRAPPER SysTick_Handler, SysTick_Handler_Body
portIRQ_WRAPPER USBHS_IRQHandler, USBHS_IRQHandler_Body
portIRQ_WRAPPER DMA1_Channel4_IRQHandler, DMA1_Channel4_IRQHandler_Body
portIRQ_WRAPPER DMA2_Channel2_IRQHandler, DMA2_Channel2_IRQHandler_Body
portIRQ_WRAPPER EXTI15_10_IRQHandler, EXTI15_10_IRQHandler_Body
portIRQ_WRAPPER TIM10_CC_IRQHandler, TIM10_CC_IRQHandler_Body
portIRQ_WRAPPER I2C2_EV_IRQHandler, I2C2_EV_IRQHandler_Body
portIRQ_WRAPPER I2C2_ER_IRQHandler, I2C2_ER_IRQHandler_Body
portIRQ_WRAPPER SDIO_IRQHandler, SDIO_IRQHandler_Body
portIRQ_WRAPPER DMA2_Channel4_IRQHandler, DMA2_Channel4_IRQHandler_Body

.global Ecall_M_Mode_Handler
.type Ecall_M_Mode_Handler, @function
.section .text.Ecall_M_Mode_Handler, "ax", @progbits
.p2align 2
Ecall_M_Mode_Handler:
    /*
     * ecall is synchronous, so the exception-context macro advances mepc past
     * the ecall before saving it in the outgoing task's context.
     */
    portcontextSAVE_EXCEPTION_CONTEXT

    /*
     * The PFIC hardware stack captured the yielding task.  Tell it that mret
     * will return with a different task's context; GIHWSTKNEN is cleared
     * automatically when the exception returns.
     */
    li t0, 0x20
    csrs 0x804, t0
    fence.i

    call vTaskSwitchContext
    portcontextRESTORE_CONTEXT
.size Ecall_M_Mode_Handler, .-Ecall_M_Mode_Handler

.global SW_Handler
.type SW_Handler, @function
.section .text.SW_Handler, "ax", @progbits
.p2align 2
SW_Handler:
    portcontextSAVE_INTERRUPT_CONTEXT

    /*
     * The PFIC hardware stack captured the interrupted task.  Tell it that
     * mret will return with a different task's context; GIHWSTKNEN is cleared
     * automatically when the interrupt returns.
     */
    li t0, 0x20
    csrs 0x804, t0
    fence.i

    /* Clear the PFIC software-interrupt pending bit. */
    li t0, 0xE000E280
    li t1, (1 << 14)
    sw t1, 0(t0)
    fence iorw, iorw

    call vTaskSwitchContext
    portcontextRESTORE_CONTEXT
.size SW_Handler, .-SW_Handler
