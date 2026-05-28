#ifndef MAIN_H
#define MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    HARDWARE_REV_UNKNOWN = 0,
    HARDWARE_REV_V1 = 1,
    HARDWARE_REV_V2 = 2,
} hardware_rev_t;

void detect_hardware_rev(void);
[[nodiscard]] hardware_rev_t get_hardware_rev(void);

#ifdef __cplusplus
}
#endif

#endif
