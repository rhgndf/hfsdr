#ifndef HFSDR_FREERTOS_TASK_STACKS_H
#define HFSDR_FREERTOS_TASK_STACKS_H

/*
 * Sizes are in 32-bit StackType_t words.  The allocations below cover the 256-byte integer/FPU
 * context-switch frame and retain 192 bytes of measured Application_Task
 * margin and 192 bytes each for usb_task and i2s_task.  Returning maskable
 * ISRs use the dedicated 2 KiB IRQ stack instead of application task stacks.
 */
#define APP_TASK_STACK_WORDS    456U
#define USB_TASK_STACK_WORDS    256U
#define I2S_TASK_STACK_WORDS    192U

#endif
