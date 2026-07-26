#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configCPU_CLOCK_HZ                         144000000UL
#define configTICK_RATE_HZ                         1000U
#define configUSE_PREEMPTION                       1
#define configUSE_TIME_SLICING                     0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION    1
#define configUSE_TICKLESS_IDLE                    1
#define configEXPECTED_IDLE_TIME_BEFORE_SLEEP      2

#define configMAX_PRIORITIES                       4
#define configMINIMAL_STACK_SIZE                   144U
#define configMAX_TASK_NAME_LEN                    16
#define configTICK_TYPE_WIDTH_IN_BITS              TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                    1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES      2
#define configQUEUE_REGISTRY_SIZE                  0
#define configENABLE_BACKWARD_COMPATIBILITY        0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS    0
#define configUSE_MINI_LIST_ITEM                   0
#define configSTACK_DEPTH_TYPE                     uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE           uint32_t
#define configUSE_NEWLIB_REENTRANT                 0

#define configUSE_TIMERS                           0
#define configUSE_EVENT_GROUPS                     0
#define configUSE_STREAM_BUFFERS                   0
#define configUSE_CO_ROUTINES                      0

#define configUSE_TASK_NOTIFICATIONS               1
#define configUSE_MUTEXES                          1
#define configUSE_RECURSIVE_MUTEXES                0
#define configUSE_COUNTING_SEMAPHORES              0
#define configUSE_QUEUE_SETS                       0

#define configSUPPORT_STATIC_ALLOCATION            1
#define configSUPPORT_DYNAMIC_ALLOCATION           0

#define configUSE_IDLE_HOOK                        0
#define configUSE_TICK_HOOK                        0
#define configCHECK_FOR_STACK_OVERFLOW             2
#define configUSE_MALLOC_FAILED_HOOK               0

#define configUSE_TRACE_FACILITY                   0
#define configGENERATE_RUN_TIME_STATS              0
#define configUSE_STATS_FORMATTING_FUNCTIONS       0
#define configUSE_POSIX_ERRNO                      0

#ifndef configENABLE_FPU
    #define configENABLE_FPU                       1
#endif
#ifndef configENABLE_VPU
    #define configENABLE_VPU                       0
#endif

/* CH32V30x uses its vendor SysTick/PFIC block, not a standard CLINT. */
#define configMTIME_BASE_ADDRESS                   0
#define configMTIMECMP_BASE_ADDRESS                0

#define INCLUDE_vTaskPrioritySet                   0
#define INCLUDE_uxTaskPriorityGet                  0
#define INCLUDE_vTaskDelete                        0
#define INCLUDE_vTaskSuspend                       1
#define INCLUDE_xTaskResumeFromISR                 1
#define INCLUDE_vTaskDelayUntil                    1
#define INCLUDE_vTaskDelay                         1
#define INCLUDE_xTaskAbortDelay                    0
#define INCLUDE_xTaskGetHandle                     0
#define INCLUDE_xTaskGetCurrentTaskHandle          1
#define INCLUDE_xTaskGetSchedulerState             0
#define INCLUDE_uxTaskGetStackHighWaterMark        1
#define INCLUDE_uxTaskGetStackHighWaterMark2       1
#define INCLUDE_eTaskGetState                      0
#define INCLUDE_xTimerPendFunctionCall             0

#ifdef __cplusplus
extern "C" {
#endif
void vFreeRTOSAssert(const char *file, int line);
#ifdef __cplusplus
}
#endif

#define configASSERT(condition)                                      \
    do                                                               \
    {                                                                \
        if(!(condition))                                             \
        {                                                            \
            vFreeRTOSAssert(__FILE__, __LINE__);                     \
        }                                                            \
    } while(0)

#endif
