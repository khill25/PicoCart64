/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Kaili Hill <kaili.hill25@gmail.com>
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pc64_regs.h"

#define SD_CARD_SECTOR_SIZE 512 // 512 bytes

extern int PC64_MCU_ID;

// UART TX buffer
extern uint16_t pc64_uart_tx_buf[PC64_BASE_ADDRESS_LENGTH];

// set the sector to start reading from
void pc64_set_sd_read_sector(uint64_t sector);

void pc64_set_sd_read_sector_part(uint index, uint16_t value);

// set the number of sectors to read
void pc64_set_sd_read_sector_count(uint32_t count);

// Set selected rom title, max 256 characters
void pc64_set_sd_rom_selection(char* title);

void pc64_send_sd_read_command(void);

bool is_sd_busy();

// UART RX methods, unique per MCU
void on_uart_rx_mcu1(void);
void on_uart_rx_mcu2(void);
// Internal method that is called once MCU2 has data ready to return MCU1
void send_data(uint32_t sector, uint32_t sectorCount);

// SD Card functions
void mount_sd(void);