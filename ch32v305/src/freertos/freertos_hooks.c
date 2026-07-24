#include "FreeRTOS.h"
#include "task.h"

static StaticTask_t s_idle_task_tcb;
static StackType_t s_idle_task_stack[configMINIMAL_STACK_SIZE]
    __attribute__((aligned(portBYTE_ALIGNMENT)));

static void freertos_halt(void) __attribute__((noreturn));

static void freertos_halt(void)
{
    portDISABLE_INTERRUPTS();
    for(;;)
    {
    }
}

void vApplicationGetIdleTaskMemory(StaticTask_t **task_tcb,
                                   StackType_t **stack_buffer,
                                   configSTACK_DEPTH_TYPE *stack_size)
{
    *task_tcb = &s_idle_task_tcb;
    *stack_buffer = s_idle_task_stack;
    *stack_size = configMINIMAL_STACK_SIZE;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    (void)task_name;
    freertos_halt();
}

void vFreeRTOSAssert(const char *file, int line)
{
    (void)file;
    (void)line;
    freertos_halt();
}
