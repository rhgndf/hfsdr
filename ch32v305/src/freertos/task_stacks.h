#ifndef HFSDR_FREERTOS_TASK_STACKS_H
#define HFSDR_FREERTOS_TASK_STACKS_H

/*
 * Sizes are in 32-bit StackType_t words.  The release/LTO -fstack-usage
 * records give worst linked call paths of 1040 bytes for Application_Task,
 * 576 bytes for usb_task, and 320 bytes for i2s_task.  The allocations below
 * also cover the maximum compiler-reported nested interrupt path, the 256-byte
 * integer/FPU context-switch frame, and margin for prebuilt newlib routines
 * whose stack frames are not present in this target's .su files.
 */
#define APP_TASK_STACK_WORDS    512U
#define USB_TASK_STACK_WORDS    384U
#define I2S_TASK_STACK_WORDS    256U

#endif
