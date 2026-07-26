#ifndef HFSDR_FREERTOS_TASK_STACKS_H
#define HFSDR_FREERTOS_TASK_STACKS_H

/*
 * Sizes are in 32-bit StackType_t words.  The allocations include the
 * 256-byte integer/FPU context-switch frame. The recording task's post-LTO
 * frame is 272 bytes. Its deepest linked path (file cleanup through FatFs and
 * asynchronous SDIO) sums to 1328 bytes before the context frame, so 432
 * words leave 144 bytes beyond that path and the port context.
 * Returning maskable ISRs use the dedicated 1.5 KiB IRQ stack instead of
 * application task stacks.
 */
#define APP_TASK_STACK_WORDS    432U
#define USB_TASK_STACK_WORDS    256U
#define I2S_TASK_STACK_WORDS    192U
#define HW_STATE_TASK_STACK_WORDS 64U
#define RECORDING_TASK_STACK_WORDS 432U

#endif
