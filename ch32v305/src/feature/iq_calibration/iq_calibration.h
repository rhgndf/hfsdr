#ifndef IQ_CALIBRATION_H
#define IQ_CALIBRATION_H

#include <optional>
#include <utility>

bool iq_calibration_run(void);
void iq_calibration_display(void);
std::optional<std::pair<float, float>> iq_calibration_measure_ready_block(void);

#endif
