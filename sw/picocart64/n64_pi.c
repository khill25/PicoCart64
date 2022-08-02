/**
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Konrad Beckmann
 */

#include "n64_pi.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/multicore.h"
#include "hardware/irq.h"

#include "n64_pi.pio.h"

#include "cic.h"
#include "n64_defs.h"
#include "pc64_regs.h"
#include "pc64_rand.h"
#include "picocart64_pins.h"
#include "picocart64.h"
#include "ringbuf.h"
#include "sram.h"
#include "stdio_async_uart.h"
#include "sd_card.h"

// The rom to load in normal .z64, big endian, format
#include "rom_vars.h"
#include "rom.h"

uint16_t rom_mapping[MAPPING_TABLE_LEN];

#if COMPRESSED_ROM
// do something
#else
static const uint16_t *rom_file_16 = (uint16_t *) rom_chunks;
#endif

RINGBUF_CREATE(ringbuf, 64, uint32_t);

// UART TX buffer
static uint16_t pc64_uart_tx_buf[PC64_BASE_ADDRESS_LENGTH];

static inline uint32_t resolve_sram_address(uint32_t address)
{
	uint32_t bank = (address >> 18) & 0x3;
	uint32_t resolved_address;

	if (bank) {
		resolved_address = address & (SRAM_256KBIT_SIZE - 1);
		resolved_address |= bank << 15;
	} else {
		resolved_address = address & (sizeof(sram) - 1);
	}

	return resolved_address;
}

static inline uint32_t n64_pi_get_value(PIO pio)
{
	uint32_t value = pio_sm_get_blocking(pio, 0);

	// Disable to get some more performance. Enable for debugging.
	// Without ringbuf, ROM access takes 160-180ns. With, 240-260ns.
#if 0
	ringbuf_add(ringbuf, value);
#elif 0
	uart_print_hex_u32(value);
#endif

	return value;
}

void n64_pi_run(void)
{
	// Init PIO
	PIO pio = pio0;
	uint offset = pio_add_program(pio, &n64_pi_program);
	n64_pi_program_init(pio, 0, offset);
	pio_sm_set_enabled(pio, 0, true);

	// Wait for reset to be released
	while (gpio_get(N64_COLD_RESET) == 0) {
		tight_loop_contents();
	}

	uint32_t last_addr;
	uint32_t addr;
	uint32_t next_word;

	// Read addr manually before the loop
	addr = n64_pi_get_value(pio);

	while (1) {
		// addr must not be a WRITE or READ request here,
		// it should contain a 16-bit aligned address.
		// Assert drains performance, uncomment when debugging.
		// ASSERT((addr != 0) && ((addr & 1) == 0));

		// We got a start address
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

			// Patch bus speed here if needed (e.g. if not overclocking)
			next_word = 0xFF40;
			// next_word = 0x2040;

			// Official SDK standard speed
			// next_word = 0x1240;
			addr = n64_pi_get_value(pio);

			// Assume addr == 0, i.e. push 16 bits of data
			pio_sm_put(pio, 0, next_word);
			last_addr += 2;

			// Pre-fetch
#if COMPRESSED_ROM
			uint32_t chunk_index = rom_mapping[(last_addr & 0xFFFFFF) >> COMPRESSION_SHIFT_AMOUNT];
			const uint16_t *chunk_16 = (const uint16_t *)rom_chunks[chunk_index];
			next_word = chunk_16[(last_addr & COMPRESSION_MASK) >> 1];
#else
			next_word = rom_file_16[(last_addr & 0xFFFFFF) >> 1];
#endif

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
			do {
				// Pre-fetch from the address
				next_word = sram[resolve_sram_address(last_addr) >> 1];

				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr & 0x00000001) {
					// We got a WRITE
					// 0bxxxxxxxx_xxxxxxxx_11111111_11111111
					sram[resolve_sram_address(last_addr) >> 1] = addr >> 16;
					last_addr += 2;
				} else if (addr == 0) {
					// READ
					pio_sm_put(pio, 0, next_word);
					last_addr += 2;
					next_word = sram[resolve_sram_address(last_addr) >> 1];
				} else {
					// New address
					break;
				}
			} while (1);
		} else if (last_addr >= 0x10000000 && last_addr <= 0x1FBFFFFF) {
			// Domain 1, Address 2 Cartridge ROM
			do {
				// Pre-fetch from the address
#if COMPRESSED_ROM
				uint32_t chunk_index = rom_mapping[(last_addr & 0xFFFFFF) >> COMPRESSION_SHIFT_AMOUNT];
				const uint16_t *chunk_16 = (const uint16_t *)rom_chunks[chunk_index];
				next_word = chunk_16[(last_addr & COMPRESSION_MASK) >> 1];
#else
				next_word = rom_file_16[(last_addr & 0xFFFFFF) >> 1];
#endif

				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr == 0) {
					// READ
 handle_d1a2_read:
					pio_sm_put(pio, 0, swap8(next_word));
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
				next_word = pc64_uart_tx_buf[(last_addr & (sizeof(pc64_uart_tx_buf) - 1)) >> 1];

				// Read command/address
				addr = n64_pi_get_value(pio);

				if (addr & 0x00000001) {
					// We got a WRITE
					// 0bxxxxxxxx_xxxxxxxx_11111111_11111111
					pc64_uart_tx_buf[(last_addr & (sizeof(pc64_uart_tx_buf) - 1)) >> 1] = swap8(addr >> 16);
					last_addr += 2;
				} else if (addr == 0) {
					// READ
					pio_sm_put(pio, 0, next_word);
					last_addr += 2;
					next_word = pc64_uart_tx_buf[(last_addr & (sizeof(pc64_uart_tx_buf) - 1)) >> 1];
				} else {
					// New address
					break;
				}
			} while (1);
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
						break;
					case PC64_COMMAND_SD_READ:
						// get data from sd card via uart to second pico
						// data will be placed into "base address memory"
						// starting at location PC64_BASE_ADDRESS_START
						// and will go for N bytes

						
					default:
						next_word = 0;
					}

					// Write as a 32-bit word
					pio_sm_put(pio, 0, next_word >> 16);
					last_addr += 2;

					// Get the next command/address
					addr = n64_pi_get_value(pio);
					if (addr != 0) {
						// Handle 16-bit reads even if we shouldn't get them here.
						continue;
					}

					pio_sm_put(pio, 0, next_word & 0xFFFF);
					last_addr += 2;
				} else if (addr & 0x00000001) {
					// WRITE

					// Read two 16-bit half-words and merge them to a 32-bit value
					uint32_t write_word = addr & 0xFFFF0000;
					write_word |= n64_pi_get_value(pio) >> 16;

					switch (last_addr - PC64_CIBASE_ADDRESS_START) {
					case PC64_REGISTER_UART_TX:
						stdio_uart_out_chars((const char *)pc64_uart_tx_buf, write_word & (sizeof(pc64_uart_tx_buf) - 1));
						break;
					case PC64_REGISTER_RAND_SEED:
						pc64_rand_seed(write_word);
						break;

					case PC64_REGISTER_SD_READ_SECTOR:
						pc64_set_sd_read_sector(write_word);
						break;

					case PC64_REGISTER_SD_READ_NUM_SECTORS:
						pc64_set_sd_read_sector_count(write_word);
						break;

					case PC64_REGISTER_SD_ROM_SELECT:
						// at this point we have already read 32 bits (4 characters)
						// each read is 16 bits (2 bytes)
						
						char rom_title[256];
						rom_title[0] = (write_word & 0xFF000000) >> 24;
						rom_title[1] = (write_word & 0x00FF0000) >> 16;
						rom_title[2] = (write_word & 0x0000FF00) >> 8;
						rom_title[3] = (write_word & 0x000000FF);

						// loop to less than size as we are doing a +1 index in the loop
						// Saves a check for boundries
						uint ni = 4;
						for (; ni < 254; ni += 2) {
							// assuming that the 32bit value returned is bits high ie
							// 0xFFFF0000 - F are the values I want
							uint16_t v = n64_pi_get_value(pio) >> 16;
							rom_title[ni] =   (v & 0xFF00) >> 8;
							rom_title[ni+1] = (v & 0x00FF);

							// If we got a null character we can exit early
							if (rom_title[ni] == 0x0) {
								break;
							} else if (rom_title[ni+1] == 0x0) {
								ni += 1; // increment the address if we also read this address
								break;
							}
						}

						// finally set the title of the selected rom
						pc64_set_sd_rom_selection(rom_title);

						// Not sure if this is needed
						last_addr += (ni - 4);
					default:
						break;
					}

					last_addr += 4;
				} else {
					// New address
					break;
				}
			} while (1);
		} else {
			// Don't handle this request - jump back to the beginning.
			// This way, there won't be a bus conflict in case e.g. a physical N64DD is connected.

			// Enable to log addresses to UART
#if 0
			uart_print_hex_u32(last_addr);
#endif

			// Read to empty fifo
			addr = n64_pi_get_value(pio);

			// Jump to start of the PIO program.
			pio_sm_exec(pio, 0, pio_encode_jmp(offset + 0));

			// Read and handle the following requests normally
			addr = n64_pi_get_value(pio);
		}
	}
}
