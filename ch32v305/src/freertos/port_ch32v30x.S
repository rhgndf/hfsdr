/*
 * CH32V30x PFIC software-interrupt bridge for the upstream FreeRTOS RISC-V
 * context port.  Task stack initialization, first-task startup, and context
 * frame save/restore remain owned by FreeRTOS-Kernel's portASM.S.
 */

#include "portContext.h"

.extern vTaskSwitchContext

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
