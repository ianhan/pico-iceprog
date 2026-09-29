#pragma once
#include "sdk.h"
#define FLASH_SIZE (2 * 1024 * 1024)
extern uint8_t model_flash[FLASH_SIZE], model_sr[2];
extern uint8_t model_sram[FLASH_SIZE];
extern size_t model_sram_bits, model_two_bit_resets;
extern bool model_loopback, model_power_down;
void model_init(void);
void model_save(void);
bool model_output(uint pin);
bool model_pull_up(uint pin);
bool model_pull_down(uint pin);
