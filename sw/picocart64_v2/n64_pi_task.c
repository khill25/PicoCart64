/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Konrad Beckmann
 * Copyright (c) 2022 Kaili Hill
 */

#include "n64_pi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <hardware/dma.h>

// #include "pico/stdlib.h"
// #include "pico/stdio.h"
#include "pico/multicore.h"
#include "hardware/irq.h"
#include "hardware/structs/systick.h"

#include "n64_defs.h"
#include "n64_pi_task.h"
#include "pc64_rand.h"
#include "pc64_regs.h"
#include "pins_mcu1.h"
#include "ringbuf.h"
#include "sram.h"
#include "stdio_async_uart.h"
#include "utils.h"

#include "sdcard/internal_sd_card.h"
#include "psram.h"
#include "rom.h"
#include "rom_vars.h"

volatile int g_currentMemoryArrayChip = START_ROM_LOAD_CHIP_INDEX;
 // Used when addressing chips outside the starting one
volatile uint32_t address_modifier = 0;
volatile bool g_loadRomFromMemoryArray = false;
static uint n64_pi_pio_offset;
volatile uint32_t tempChip = 0;

volatile uint16_t *ptr16 = (volatile uint16_t *)0x13000000; // no cache
volatile int dma_chan = -1;
volatile int dma_chan_high = -1;
volatile int sram_dma_chan = -1;
volatile int sram_dma_write_chan = -1;
// volatile uint16_t *dmaBuffer;
// volatile uint16_t *dmaBufferHigh;
volatile uint16_t dma_bi = 0;
// volatile uint16_t sram_dma_buffer;

uint16_t rom_mapping[MAPPING_TABLE_LEN];

#if COMPRESSED_ROM
// do something
#else
static const uint16_t *rom_file_16 = (uint16_t *) rom_chunks;
#endif

// Num bytes to offset from address based on chip index
#define PSRAM_ADDRESS_MODIFIER_1 (0)
#define PSRAM_ADDRESS_MODIFIER_2 (PSRAM_CHIP_CAPACITY_BYTES)
#define PSRAM_ADDRESS_MODIFIER_3 (PSRAM_CHIP_CAPACITY_BYTES * 2) 
#define PSRAM_ADDRESS_MODIFIER_4 (PSRAM_CHIP_CAPACITY_BYTES * 3) 
#define PSRAM_ADDRESS_MODIFIER_5 (PSRAM_CHIP_CAPACITY_BYTES * 4) 
#define PSRAM_ADDRESS_MODIFIER_6 (PSRAM_CHIP_CAPACITY_BYTES * 5) 
#define PSRAM_ADDRESS_MODIFIER_7 (PSRAM_CHIP_CAPACITY_BYTES * 6)
#define PSRAM_ADDRESS_MODIFIER_8 (PSRAM_CHIP_CAPACITY_BYTES * 7)
uint32_t g_addressModifierTable[] = {
	0, // no chip 0
	PSRAM_ADDRESS_MODIFIER_1, // start at chip 1
	PSRAM_ADDRESS_MODIFIER_2,
	PSRAM_ADDRESS_MODIFIER_3,
	PSRAM_ADDRESS_MODIFIER_4,
	PSRAM_ADDRESS_MODIFIER_5,
	PSRAM_ADDRESS_MODIFIER_6,
	PSRAM_ADDRESS_MODIFIER_7,
	PSRAM_ADDRESS_MODIFIER_8
};

// static inline uint32_t resolve_sram_address(uint32_t address)
// {	
// 	uint32_t bank = (address >> 18) & 0x3;
// 	uint32_t resolved_address;

// 	if (bank) {
// 		resolved_address = address & (SRAM_256KBIT_SIZE - 1);
// 		resolved_address |= bank << 15;
// 	} else {
// 		resolved_address = address & (sizeof(sram) - 1);
// 	}

// 	return resolved_address;
// }

#define SRAM_SIZE_MASK 0x7FFF
#define COMBINED_MASK (SRAM_SIZE_MASK | 0x18000) // 0x1FFFF
static inline uint32_t resolve_sram_address(uint32_t address)
{
    return (address & SRAM_SIZE_MASK) | ((address & 0xC0000) >> 3);
}

static inline uint32_t n64_pi_get_value(PIO pio)
{
	uint32_t value = pio_sm_get_blocking(pio, 0);
	return value;
}

void __no_inline_not_in_flash_func(array_test_method)(void) {
	volatile uint32_t sramValue = 0;
	volatile uint32_t sramAddr = 0;
	volatile uint16_t* sramPtr = (volatile uint16_t* )sram;
	uint32_t st = time_us_32();
	for(volatile int i = 0; i < 1000000; i++) {
		// sramValue = sram[((sramAddr & SRAM_SIZE_MASK) | ((sramAddr & 0xC0000) >> 3)) >> 1];
		// sramValue = 0;//*(sramPtr + sramAddr);//sram[sramAddr];
	}
	uint32_t tt = time_us_32() - st;
	printf("%uus \n", tt);
}

void __no_inline_not_in_flash_func(n64_pi_run)(void)
{
	systick_hw->csr = 0x5;
    systick_hw->rvr = 0x00FFFFFF;

	// Probably already restarted or first time start, we want to run the loop
	// until this is true, so always reset it
	g_restart_pi_handler = false;

	g_currentMemoryArrayChip = START_ROM_LOAD_CHIP_INDEX;

	// Init PIO
	PIO pio = pio0;
	n64_pi_pio_offset = pio_add_program(pio, &n64_pi_program);
	n64_pi_program_init(pio, 0, n64_pi_pio_offset);
	pio_sm_set_enabled(pio, 0, true);

	dma_chan = dma_claim_unused_channel(true);
	dma_channel_config c = dma_channel_get_default_config(dma_chan);
	// channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	channel_config_set_read_increment(&c, true);
	// channel_config_set_write_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_bswap(&c, true);
	channel_config_set_high_priority(&c, true);

	// volatile uint16_t dmaValue = 0;
	// uint16_t* dmaBuffer;
	// dmaBuffer = malloc(sizeof(uint32_t) * 4); // 4 16 bit values
	dma_channel_configure(
		dma_chan,        // Channel to be configured
		&c,              // The configuration we just created
		// &dmaValue,
		pc64_uart_tx_buf,      // The initial write address
		ptr16,           // The initial read address
		1, 				 // Number of transfers;
		false           
	);
	//pc64_uart_tx_buf

	// sram_dma_chan = dma_claim_unused_channel(true);
	// dma_channel_config sram_dma_config = dma_channel_get_default_config(sram_dma_chan);
	// channel_config_set_transfer_data_size(&sram_dma_config, DMA_SIZE_16);
	// channel_config_set_read_increment(&sram_dma_config, true);
	// channel_config_set_write_increment(&sram_dma_config, false);
	// // channel_config_set_bswap(&c, true);
	// // channel_config_set_high_priority(&c, true);

	// dma_channel_configure(
	// 	sram_dma_chan,        // Channel to be configured
	// 	&sram_dma_config,              // The configuration we just created
	// 	&sram_dma_buffer,       // The initial write address //&pio0->txf[0]
	// 	sram,           // The initial read address
	// 	1, 				 // Number of transfers;
	// 	false           
	// );

	// sram_dma_write_chan = dma_claim_unused_channel(true);
	// dma_channel_config sram_dma_write_config = dma_channel_get_default_config(sram_dma_write_chan);
	// channel_config_set_transfer_data_size(&sram_dma_write_config, DMA_SIZE_16);
	// channel_config_set_read_increment(&sram_dma_write_config, false);
	// channel_config_set_write_increment(&sram_dma_write_config, true);

	// dma_channel_configure(
	// 	sram_dma_write_chan,     // Channel to be configured
	// 	&sram_dma_write_config,  // The configuration we just created
	// 	sram,      				 // The initial write address //&pio0->txf[0]
	// 	&sram_dma_buffer, 		// The initial read address
	// 	1, 				 		// Number of transfers;
	// 	false           
	// );

	// uint32_t sram_dma_trigger = 1u << sram_dma_chan;
	// uint32_t sram_dma_write_trigger = 1u << sram_dma_write_chan;

	// array_test_method();

	// Wait for reset to be released
	while (gpio_get(PIN_N64_COLD_RESET) == 0) {
		tight_loop_contents(); 
	}

	// dma_channel_set_read_addr(dma_chan, ptr16 + (((0 - g_addressModifierTable[g_currentMemoryArrayChip]) & 0xFFFFFF) >> 1), false);
	// dma_hw->multi_channel_trigger = 1u << dma_chan;
	// while(!!(dma_hw->ch[dma_chan].al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS)) { tight_loop_contents(); }
	// printf("%04x %04x\n", pc64_uart_tx_buf[0], pc64_uart_tx_buf[1]);

	// dma_channel_set_read_addr(dma_chan, ptr16 + ((((0x10000040) - g_addressModifierTable[g_currentMemoryArrayChip]) & 0xFFFFFF) >> 1), false);
	// dma_hw->multi_channel_trigger = 1u << dma_chan;

	// volatile uint32_t* buf32 = (volatile uint32_t*)pc64_uart_tx_buf;
	// for(int i = 0; i < 32; i += 2) {
	// 	while(!!(dma_hw->ch[dma_chan].al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS)) { tight_loop_contents(); } // dma_channel_wait_for_finish_blocking(dma_chan);
	// 	if(i % 4 == 0) { printf("\n(%08x): ", i); }
		
	// 	printf("(%08x) ", buf32[i/2]);
	// 	printf("%04x ", swap8(pc64_uart_tx_buf[dma_bi++]));
	// 	printf("%04x ", swap8(pc64_uart_tx_buf[dma_bi++]));

	// 	dma_hw->multi_channel_trigger = 1u << dma_chan;
	// }

	volatile uint32_t last_addr;
	volatile uint32_t addr;
	volatile uint32_t next_word;
	volatile uint32_t startTicks = 0;
	volatile uint32_t sram_addr = 0;
	
	// Read addr manually before the loop
	addr = n64_pi_get_value(pio);

	uint32_t lastUpdate = 0;
	while (1 && !g_restart_pi_handler) {
		// addr must not be a WRITE or READ request here,
		// it should contain a 16-bit aligned address.
		// Address aquired
		last_addr = addr;

		// Handle access based on memory region
		// Note that the if-cases are ordered in priority from
		// most timing critical to least.
		if (last_addr == 0x10000000) {
			// Configure bus to run slowly.
			// This is better patched in the rom, so we won't need a branch here.
			// But let's keep it here so it's easy to import roms.

			// 0x8037FF40 in big-endian
			next_word = 0x8037;
			addr = n64_pi_get_value(pio);

			// Assume addr == 0, i.e. READ request
			pio_sm_put(pio, 0, next_word);
			last_addr += 2;

			// Patch bus speed here if needed 
			// next_word = 0xFF40; // Slowest speed
			// next_word = 0x8040; // boots @ 266MHz
			// next_word = 0x4040; // boots @ 266
			// next_word = 0x3040; // boots @ 266
			// next_word = 0x2040; // Should boot with rp2040's @ 360MHz (qspi at 90MHz)
			// next_word = 0x1B40; 
			
			//0x1B40 boots@266/4 with dmaValue
			//0x1A40 no boot@266/4 with dmaValue
			// next_word = 0x1A40; 

			// next_word = 0x1940; 
			// next_word = 0x1840;
			// next_word = 0x1740;
			next_word = 0x1640; // boots @ 300 (psram divider = 4)
			// next_word = 0x1540;
			// next_word = 0x1440;
			// next_word = 0x1340;
			// next_word = 0x1240; // Only usable if psram/flash is readable at 133MHz
		
			addr = n64_pi_get_value(pio);

			// Assume addr == 0, i.e. push 16 bits of data
			pio_sm_put(pio, 0, next_word);
			last_addr += 2;

			// If we are loading data from psram, use dma, otherwise just use the array in flash.
			// if (g_loadRomFromMemoryArray) {
				// dma_channel_set_read_addr(dma_chan, ptr16 + (((last_addr - g_addressModifierTable[g_currentMemoryArrayChip]) & 0xFFFFFF) >> 1), false);
			(&dma_hw->ch[dma_chan])->al3_read_addr_trig = (uintptr_t)(ptr16 + (((last_addr - g_addressModifierTable[g_currentMemoryArrayChip]) & 0xFFFFFF) >> 1));
			while(!!(dma_hw->ch[dma_chan].al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS)) { tight_loop_contents(); }
			dma_hw->multi_channel_trigger = 1u << dma_chan;
			// } else {
			// 	uint32_t chunk_index = rom_mapping[(last_addr & 0xFFFFFF) >> COMPRESSION_SHIFT_AMOUNT];
			// 	const uint16_t *chunk_16 = (const uint16_t *)rom_chunks[chunk_index];
			// 	next_word = chunk_16[(last_addr & COMPRESSION_MASK) >> 1];
			// }
			
			// ROM patching done
			addr = n64_pi_get_value(pio);
			if (addr == 0) {
				// I apologise for the use of goto, but it seemed like a fast way
				// to enter the next state immediately.
				goto handle_d1a2_read;
			} else {
				continue;
			}
		} else if (last_addr >= CART_SRAM_START && last_addr <= CART_SRAM_END) {
			// Domain 2, Address 2 Cartridge SRAM
			sram_addr = (last_addr & 0x7FFF) >> 1;;// ((last_addr & SRAM_SIZE_MASK) | ((last_addr & 0xC0000) >> 3)) >> 1;
			next_word = sram[sram_addr];

			// dma_channel_set_write_addr(sram_dma_write_chan, sram + sram_addr, false);
			// (&dma_hw->ch[sram_dma_write_chan])->write_addr = (uintptr_t)(sram + sram_addr);

			// dma_channel_set_read_addr(sram_dma_chan, sram + sram_addr, false);
			// dma_hw->multi_channel_trigger = sram_dma_trigger;
			
			do {
				// Read command/address
				// addr = n64_pi_get_value(pio);
				while((pio->fstat & 0x100) != 0) { tight_loop_contents(); }
				addr = pio->rxf[0];

				if (addr & 0x00000001) {
					// We got a WRITE
					// 0bxxxxxxxx_xxxxxxxx_11111111_11111111
					// sram_dma_buffer = addr >> 16;
					// dma_hw->multi_channel_trigger = sram_dma_write_trigger;
					// last_addr += 2;

					sram[sram_addr++] = addr >> 16;
					last_addr += 2;
				} else if (addr == 0) {
					// READ
					// pio->txf[0] = sram_dma_buffer;
					// last_addr += 2;
					// dma_hw->multi_channel_trigger = sram_dma_trigger;

					pio->txf[0] = next_word;
					last_addr += 2;
					next_word = sram[++sram_addr];
					
				} else {
					// New address
					break;
				}
			} while (1);
		} else if (last_addr >= 0x10000000 && last_addr <= 0x1FBFFFFF) {
			// Domain 1, Address 2 Cartridge ROM

			// Change the banked memory chip if needed
			tempChip = psram_addr_to_chip(last_addr);
			if (tempChip != g_currentMemoryArrayChip) {
				g_currentMemoryArrayChip = tempChip;
				// Set the new chip
				psram_set_cs(g_currentMemoryArrayChip);
			}

			dma_bi = 0;// reset the buffer index
			// Set the correct read address
			(&dma_hw->ch[dma_chan])->read_addr = (uintptr_t)(ptr16 + (((last_addr - g_addressModifierTable[g_currentMemoryArrayChip]) & 0xFFFFFF) >> 1));
			(&dma_hw->ch[dma_chan])->al2_write_addr_trig = (uintptr_t) pc64_uart_tx_buf;
			while(!!(dma_hw->ch[dma_chan].al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS)) { tight_loop_contents(); } // dma_channel_wait_for_finish_blocking(dma_chan);
			dma_hw->multi_channel_trigger = 1u << dma_chan;

			do {	
				
				while((pio->fstat & 0x100) != 0) tight_loop_contents();
				addr = pio->rxf[0];

				if (addr == 0) {
					// READ
 handle_d1a2_read:
 					pio->txf[0] = pc64_uart_tx_buf[dma_bi++];
					dma_hw->multi_channel_trigger = 1u << dma_chan;
					last_addr += 2;

				} else if (addr & 0x00000001) {
					// WRITE
					// Ignore data since we're asked to write to the ROM.
					last_addr += 2;
				} else {
					// New address
					break;
				}
			} while (1);
		}
#if 0
		else if (last_addr >= 0x05000000 && last_addr <= 0x05FFFFFF) {
			// Domain 2, Address 1 N64DD control registers
			do {
				// We don't support this yet, but we have to consume another value
				next_word = 0;

				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr == 0) {
					// READ
					pio_sm_put(pio, 0, next_word);
					last_addr += 2;
				} else if (addr & 0x00000001) {
					// WRITE
					// Ignore
					last_addr += 2;
				} else {
					// New address
					break;
				}
			} while (1);
		} else if (last_addr >= 0x06000000 && last_addr <= 0x07FFFFFF) {
			// Domain 1, Address 1 N64DD IPL ROM (if present)
			do {
				// We don't support this yet, but we have to consume another value
				next_word = 0;

				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr == 0) {
					// READ
					pio_sm_put(pio, 0, next_word);
					last_addr += 2;
				} else if (addr & 0x00000001) {
					// WRITE
					// Ignore
					last_addr += 2;
				} else {
					// New address
					break;
				}
			} while (1);
		}
#endif
		else if (last_addr >= PC64_BASE_ADDRESS_START && last_addr <= PC64_BASE_ADDRESS_END) {
			// PicoCart64 BASE address space
			do {
				// Pre-fetch from the address
				uint32_t buf_index = (last_addr & (sizeof(pc64_uart_tx_buf) - 1)) >> 1;
				//next_word = PC64_MAGIC;//swap8(pc64_uart_tx_buf[buf_index]);

				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr & 0x00000001) {
					// We got a WRITE
					// 0bxxxxxxxx_xxxxxxxx_11111111_11111111
					// pc64_uart_tx_buf[(last_addr & (sizeof(pc64_uart_tx_buf) - 1)) >> 1] = swap8(addr >> 16);
					pc64_uart_tx_buf[buf_index] = swap8(addr >> 16);
					last_addr += 2;
				} else if (addr == 0) {
					// READ
					next_word = pc64_uart_tx_buf[buf_index];
					pio_sm_put(pio, 0, next_word);
					last_addr += 2;

				} else {
					// New address
					break;
				}

				if (g_restart_pi_handler) {
					break;
				}
			} while (1);
		
		} else if (last_addr >= PC64_CIBASE_ADDRESS_START && last_addr <= PC64_CIBASE_ADDRESS_END) {
			// PicoCart64 CIBASE address space
			do {
				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr == 0) {
					// READ
					switch (last_addr - PC64_CIBASE_ADDRESS_START) {
					case PC64_REGISTER_MAGIC:
						next_word = PC64_MAGIC;

						// Write as a 32-bit word
						pio_sm_put(pio, 0, next_word >> 16);
						last_addr += 2;
						// Get the next command/address
						addr = n64_pi_get_value(pio);
						if (addr != 0) {
							continue;
						}

						pio_sm_put(pio, 0, next_word & 0xFFFF);

						break;
					case PC64_REGISTER_SD_BUSY:
						// next_word = sd_is_busy ? 0x00000001 : 0x00000000;
						
						// Upper 16 bits are just 0
						pio_sm_put(pio, 0, 0x0000);

						// last_addr += 2;

						// // Get the next command/address
						// addr = n64_pi_get_value(pio);
						// if (addr != 0) {
						// 	continue;
						// }

						// // now we can send the actual busy bit
						// if (sd_is_busy) {
						// 	pio_sm_put(pio, 0, 0x0001);
						// } else {
						// 	pio_sm_put(pio, 0, 0x0000);
						// }
						
						break;
					case (PC64_REGISTER_SD_BUSY + 2):
						if (sd_is_busy) {
							pio_sm_put(pio, 0, 0x0001);
						} else {
							pio_sm_put(pio, 0, 0x0000);
						}
						break;

					default:
						next_word = 0;
					}

					last_addr += 2;
					
				} else if (addr & 0x00000001) {
					// WRITE

					// Read two 16-bit half-words and merge them to a 32-bit value
					uint32_t write_word = addr & 0xFFFF0000;
					// uint16_t half_word = addr >> 16;
					uint addr_advance = 2;

					switch (last_addr - PC64_CIBASE_ADDRESS_START) {
					case PC64_REGISTER_UART_TX:
						write_word |= n64_pi_get_value(pio) >> 16;
						//stdio_uart_out_chars((const char *)pc64_uart_tx_buf, write_word & (sizeof(pc64_uart_tx_buf) - 1));
						addr_advance = 4;
						break;

					case PC64_REGISTER_RAND_SEED:
						write_word |= n64_pi_get_value(pio) >> 16;
						pc64_rand_seed(write_word);
						addr_advance = 4;
						break;

					case PC64_COMMAND_SD_READ:
						// write_word |= n64_pi_get_value(pio) >> 16;
						// multicore_fifo_push_blocking(CORE1_SEND_SD_READ_CMD);
						break;

					case (PC64_COMMAND_SD_READ + 2):
						multicore_fifo_push_blocking(CORE1_SEND_SD_READ_CMD);
						break;

					case PC64_REGISTER_SD_READ_SECTOR0:
						// write_word |= n64_pi_get_value(pio) >> 16;
						// pc64_set_sd_read_sector_part(0, write_word);
						pc64_set_sd_read_sector_part(0, write_word);
						break;

					case (PC64_REGISTER_SD_READ_SECTOR0+2):
						pc64_set_sd_read_sector_part(1, write_word);
						break;

					case PC64_REGISTER_SD_READ_SECTOR1:
						// write_word |= n64_pi_get_value(pio) >> 16;
						// pc64_set_sd_read_sector_part(1, write_word);
						pc64_set_sd_read_sector_part(2, write_word);
						break;

					case (PC64_REGISTER_SD_READ_SECTOR1 + 2):
						pc64_set_sd_read_sector_part(3, write_word);
						break;

					case PC64_REGISTER_SD_READ_NUM_SECTORS:
						// write_word |= n64_pi_get_value(pio) >> 16;
						// pc64_set_sd_read_sector_count(1, write_word);
						pc64_set_sd_read_sector_count(1, write_word);
						break;

					case (PC64_REGISTER_SD_READ_NUM_SECTORS + 2):
						pc64_set_sd_read_sector_count(0, write_word);
						break;

					case PC64_REGISTER_SD_SELECT_ROM:
						// write_word |= n64_pi_get_value(pio) >> 16;
						pc64_set_sd_rom_selection_length_register(write_word, 0);
						break;

					case (PC64_REGISTER_SD_SELECT_ROM + 2):
						pc64_set_sd_rom_selection_length_register(write_word, 1);
						pc64_set_sd_rom_selection((char *)pc64_uart_tx_buf, write_word);
						multicore_fifo_push_blocking(CORE1_LOAD_NEW_ROM_CMD);
						break;

					default:
						break;
					}

					last_addr += addr_advance;
				} else {
					// New address
					break;
				}

				if (g_restart_pi_handler) {
					break;
				}
			} while (1);
		} else if (last_addr >= 0x81000000 && last_addr <= 0x81001000) {
			uart_tx_program_putc(0x09);
			uart_tx_program_putc(0x08);
			uart_tx_program_putc(0x07);
			// Read to empty fifo
			addr = n64_pi_get_value(pio);

			// Jump to start of the PIO program.
			pio_sm_exec(pio, 0, pio_encode_jmp(n64_pi_pio_offset + 0));

			// Read and handle the following requests normally
			addr = n64_pi_get_value(pio);
		} else if (last_addr >= PC64_RAND_ADDRESS_START && last_addr <= PC64_RAND_ADDRESS_END) {
			// PicoCart64 RAND address space
			do {
				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr & 0x00000001) {
					// We got a WRITE
					last_addr += 2;
				} else if (addr == 0) {
					// READ
					next_word = pc64_rand16();
					pio_sm_put(pio, 0, next_word);
					last_addr += 2;
				} else {
					// New address
					break;
				}

				if (g_restart_pi_handler) {
					break;
				}
			} while (1);
		} else {
			// Don't handle this request - jump back to the beginning.
			// This way, there won't be a bus conflict in case e.g. a physical N64DD is connected.
			// Read to empty fifo
			addr = n64_pi_get_value(pio);

			// Jump to start of the PIO program.
			pio_sm_exec(pio, 0, pio_encode_jmp(n64_pi_pio_offset + 0));

			// Read and handle the following requests normally
			addr = n64_pi_get_value(pio);
		}
	}

	// Tear down the pio sm so the function can be called again.
	pio_sm_set_enabled(pio, 0, false);
	pio_remove_program(pio, &n64_pi_program, n64_pi_pio_offset);
}
