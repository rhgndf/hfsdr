#ifndef PORTMACRO_H
#define PORTMACRO_H

#ifdef __cplusplus
extern "C" {
#endif

#if __riscv_xlen != 32
    #error "The CH32V30x FreeRTOS port requires RV32"
#endif

#define portSTACK_TYPE           uint32_t
#define portBASE_TYPE            int32_t
#define portUBASE_TYPE           uint32_t
#define portPOINTER_SIZE_TYPE    uint32_t
#define portMAX_DELAY            ((TickType_t)0xffffffffUL)

typedef portSTACK_TYPE StackType_t;
typedef portBASE_TYPE BaseType_t;
typedef portUBASE_TYPE UBaseType_t;
typedef portUBASE_TYPE TickType_t;

#define portCHAR                 char
#define portFLOAT                float
#define portDOUBLE               double
#define portLONG                 long
#define portSHORT                short

#define portTICK_TYPE_IS_ATOMIC  1
#define portSTACK_GROWTH         (-1)
#define portTICK_PERIOD_MS       ((TickType_t)1000U / configTICK_RATE_HZ)
#define portBYTE_ALIGNMENT       16
#define portCRITICAL_NESTING_IN_TCB 0
#define portHAS_NESTED_INTERRUPTS    1

void vPortYield(void);
void vPortSuppressTicksAndSleep(TickType_t expected_idle_time);

#define portYIELD()              vPortYield()
#define portYIELD_FROM_ISR(switch_required)                      \
    do                                                           \
    {                                                            \
        if((switch_required) != pdFALSE)                         \
        {                                                        \
            vPortYield();                                        \
        }                                                        \
    } while(0)
#define portEND_SWITCHING_ISR(switch_required) \
    portYIELD_FROM_ISR(switch_required)

static inline UBaseType_t ulPortSetInterruptMask(void)
{
    UBaseType_t previous_mstatus;

    __asm volatile(
        "csrrc %0, mstatus, %1\n"
        "fence.i"
        : "=r"(previous_mstatus)
        : "r"(0x8U)
        : "memory");

    return previous_mstatus & 0x8U;
}

static inline void vPortClearInterruptMask(UBaseType_t previous_mie)
{
    if((previous_mie & 0x8U) != 0U)
    {
        __asm volatile(
            "csrs mstatus, %0\n"
            "fence.i"
            :
            : "r"(0x8U)
            : "memory");
    }
    else
    {
        __asm volatile(
            "csrc mstatus, %0\n"
            "fence.i"
            :
            : "r"(0x8U)
            : "memory");
    }
}

#define portSET_INTERRUPT_MASK()             ulPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK(mask)       vPortClearInterruptMask(mask)
#define portSET_INTERRUPT_MASK_FROM_ISR()    ulPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(mask) vPortClearInterruptMask(mask)

#define portDISABLE_INTERRUPTS()                                      \
    __asm volatile("csrc mstatus, %0\nfence.i" :: "r"(0x8U) : "memory")
#define portENABLE_INTERRUPTS()                                       \
    __asm volatile("csrs mstatus, %0\nfence.i" :: "r"(0x8U) : "memory")

extern size_t xCriticalNesting;

#define portENTER_CRITICAL()                                          \
    do                                                               \
    {                                                                \
        portDISABLE_INTERRUPTS();                                    \
        xCriticalNesting++;                                          \
    } while(0)

#define portEXIT_CRITICAL()                                           \
    do                                                               \
    {                                                                \
        configASSERT(xCriticalNesting > 0U);                          \
        xCriticalNesting--;                                          \
        if(xCriticalNesting == 0U)                                   \
        {                                                            \
            portENABLE_INTERRUPTS();                                 \
        }                                                            \
    } while(0)

#ifndef configUSE_PORT_OPTIMISED_TASK_SELECTION
    #define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#endif

#if (configUSE_PORT_OPTIMISED_TASK_SELECTION == 1)
    #if (configMAX_PRIORITIES > 32)
        #error "Optimised task selection supports at most 32 priorities"
    #endif
    #define portRECORD_READY_PRIORITY(priority, ready_priorities) \
        ((ready_priorities) |= (1UL << (priority)))
    #define portRESET_READY_PRIORITY(priority, ready_priorities)  \
        ((ready_priorities) &= ~(1UL << (priority)))
    #define portGET_HIGHEST_PRIORITY(top_priority, ready_priorities) \
        ((top_priority) = (31UL - (uint32_t)__builtin_clz(ready_priorities)))
#endif

#define portTASK_FUNCTION_PROTO(function, parameters) \
    void function(void *parameters)
#define portTASK_FUNCTION(function, parameters) \
    void function(void *parameters)

#define portNOP()                   __asm volatile("nop")
#define portINLINE                  __inline
#define portFORCE_INLINE            inline __attribute__((always_inline))
#define portMEMORY_BARRIER()        __asm volatile("" ::: "memory")
#define portSUPPRESS_TICKS_AND_SLEEP(expected_idle_time) \
    vPortSuppressTicksAndSleep(expected_idle_time)

#if !defined(configMTIME_BASE_ADDRESS) || !defined(configMTIMECMP_BASE_ADDRESS)
    #error "FreeRTOSConfig.h must define both MTIME addresses as zero"
#endif

#ifdef __cplusplus
}
#endif

#endif
