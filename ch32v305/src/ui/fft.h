#ifndef UI_FFT_H
#define UI_FFT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void UI_FFT_Init(void);
void UI_FFT_Compute(void);
void UI_FFT_Draw(void);
const float *UI_FFT_Buffer(void);
uint32_t UI_FFT_BinCount(void);

#ifdef __cplusplus
}
#endif

#endif
