#ifndef CST328_H
#define CST328_H

#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

ErrorStatus cst328_hw_init(void);
void cst328_hw_poll(void);
void cst328_hw_print_details(void);

#ifdef __cplusplus
}
#endif

#endif
