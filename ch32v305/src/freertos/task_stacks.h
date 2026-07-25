#ifndef HFSDR_FREERTOS_TASK_STACKS_H
#define HFSDR_FREERTOS_TASK_STACKS_H

/*
 * Sizes are in 32-bit StackType_t words.  The allocations include the
 * 256-byte integer/FPU context-switch frame.  Application_Task was reduced by
 * the 96 bytes removed from its compiler-emitted frame when SDCardPoll was
 * disabled; USB and I2S retain their existing margins.  Returning maskable
 * ISRs use the dedicated 1.5 KiB IRQ stack instead of application task stacks.
 */
#define APP_TASK_STACK_WORDS    432U
#define USB_TASK_STACK_WORDS    256U
#define I2S_TASK_STACK_WORDS    192U

#endif
