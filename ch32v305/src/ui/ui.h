#ifndef UI_UI_H
#define UI_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void UI_Init(void);
void UI_Draw(void);
bool UI_ShouldDrawFft(void);

/* Called from the encoder EXTI ISR; these handlers must remain non-blocking. */
void ui_handle_button_press(void);
void ui_handle_button_long_press(void);

#ifdef __cplusplus
}
#endif

#endif
