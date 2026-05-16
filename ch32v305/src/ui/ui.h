#ifndef UI_UI_H
#define UI_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void UI_Init(void);
void UI_Draw(void);
bool UI_ShouldDrawFft(void);

#ifdef __cplusplus
}
#endif

#endif
