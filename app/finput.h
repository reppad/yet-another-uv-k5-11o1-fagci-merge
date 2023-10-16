#ifndef FINPUT_H

#define FINPUT_H

#include "../driver/keyboard.h"
#include <stdint.h>

extern const uint8_t FREQ_INPUT_LENGTH;
extern char freqInputString[];
extern uint8_t freqInputIndex;
extern uint32_t tempFreq;

void UpdateFreqInput(key_code_t key);
void ResetFreqInput();
void FreqInput();

#endif /* end of include guard: FINPUT_H */

// vim: ft=c
