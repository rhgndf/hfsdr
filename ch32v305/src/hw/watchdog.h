#ifndef WATCHDOG_H
#define WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

void watchdog_init(void);
void watchdog_kick(void);

#ifdef __cplusplus
}
#endif

#endif
