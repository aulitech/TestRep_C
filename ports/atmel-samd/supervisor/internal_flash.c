/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "supervisor/internal_flash.h"

#include <stdint.h>
#include <string.h>

#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "lib/oofatfs/ff.h"

#ifdef SAMD21
#include "hpl/pm/hpl_pm_base.h"
#endif
#include "hal/include/hal_flash.h"

#include "supervisor/shared/rgb_led_status.h"

static struct flash_descriptor supervisor_flash_desc;

void supervisor_flash_init(void) {
    // Activity LED for flash writes.
    #ifdef MICROPY_HW_LED_MSC
        struct port_config pin_conf;
        port_get_config_defaults(&pin_conf);

        pin_conf.direction  = PORT_PIN_DIR_OUTPUT;
        port_pin_set_config(MICROPY_HW_LED_MSC, &pin_conf);
        port_pin_set_output_level(MICROPY_HW_LED_MSC, false);
    #endif

    #ifdef SAMD51
    hri_mclk_set_AHBMASK_NVMCTRL_bit(MCLK);
    #endif
    #ifdef SAMD21
    _pm_enable_bus_clock(PM_BUS_APBB, NVMCTRL);
    #endif
    flash_init(&supervisor_flash_desc, NVMCTRL);
}

uint32_t supervisor_flash_get_block_size(void) {
    return FILESYSTEM_BLOCK_SIZE;
}

uint32_t supervisor_flash_get_block_count(void) {
    return INTERNAL_FLASH_PART1_START_BLOCK + INTERNAL_FLASH_PART1_NUM_BLOCKS;
}

void supervisor_flash_flush(void) {
}

void flash_flush(void) {
    supervisor_flash_flush();
}

static void build_partition(uint8_t *buf, int boot, int type, uint32_t start_block, uint32_t num_blocks) {
    buf[0] = boot;

    if (num_blocks == 0) {
        buf[1] = 0;
        buf[2] = 0;
        buf[3] = 0;
    } else {
        buf[1] = 0xff;
        buf[2] = 0xff;
        buf[3] = 0xff;
    }

    buf[4] = type;

    if (num_blocks == 0) {
        buf[5] = 0;
        buf[6] = 0;
        buf[7] = 0;
    } else {
        buf[5] = 0xff;
        buf[6] = 0xff;
        buf[7] = 0xff;
    }

    buf[8] = start_block;
    buf[9] = start_block >> 8;
    buf[10] = start_block >> 16;
    buf[11] = start_block >> 24;

    buf[12] = num_blocks;
    buf[13] = num_blocks >> 8;
    buf[14] = num_blocks >> 16;
    buf[15] = num_blocks >> 24;
}

static int32_t convert_block_to_flash_addr(uint32_t block) {
    if (INTERNAL_FLASH_PART1_START_BLOCK <= block && block < INTERNAL_FLASH_PART1_START_BLOCK + INTERNAL_FLASH_PART1_NUM_BLOCKS) {
        // a block in partition 1
        block -= INTERNAL_FLASH_PART1_START_BLOCK;
        return INTERNAL_FLASH_MEM_SEG1_START_ADDR + block * FILESYSTEM_BLOCK_SIZE;
    }
    // bad block
    return -1;
}

bool supervisor_flash_read_block(uint8_t *dest, uint32_t block) {
    if (block == 0) {
        // fake the MBR so we can decide on our own partition table

        for (int i = 0; i < 446; i++) {
            dest[i] = 0;
        }

        build_partition(dest + 446, 0, 0x01 /* FAT12 */, INTERNAL_FLASH_PART1_START_BLOCK, INTERNAL_FLASH_PART1_NUM_BLOCKS);
        build_partition(dest + 462, 0, 0, 0, 0);
        build_partition(dest + 478, 0, 0, 0, 0);
        build_partition(dest + 494, 0, 0, 0, 0);

        dest[510] = 0x55;
        dest[511] = 0xaa;

        return true;

    } else {
        // non-MBR block, get data from flash memory
        int32_t src = convert_block_to_flash_addr(block);
        if (src == -1) {
            // bad block number
            return false;
        }
        int32_t error_code = flash_read(&supervisor_flash_desc, src, dest, FILESYSTEM_BLOCK_SIZE);
        return error_code == ERR_NONE;
    }
}

bool supervisor_flash_write_block(const uint8_t *src, uint32_t block) {
    if (block == 0) {
        // can't write MBR, but pretend we did
        return true;

    } else {
        #ifdef MICROPY_HW_LED_MSC
            port_pin_set_output_level(MICROPY_HW_LED_MSC, true);
        #endif
        temp_status_color(ACTIVE_WRITE);
        // non-MBR block, copy to cache
        int32_t dest = convert_block_to_flash_addr(block);
        if (dest == -1) {
            // bad block number
            return false;
        }
        int32_t error_code;
        error_code = flash_erase(&supervisor_flash_desc,
                                 dest,
                                 FILESYSTEM_BLOCK_SIZE / flash_get_page_size(&supervisor_flash_desc));
        if (error_code != ERR_NONE) {
            return false;
        }

        error_code = flash_append(&supervisor_flash_desc, dest, src, FILESYSTEM_BLOCK_SIZE);
        if (error_code != ERR_NONE) {
            return false;
        }
        clear_temp_status();
        #ifdef MICROPY_HW_LED_MSC
            port_pin_set_output_level(MICROPY_HW_LED_MSC, false);
        #endif
        return true;
    }
}

mp_uint_t supervisor_flash_read_blocks(uint8_t *dest, uint32_t block_num, uint32_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) {
        if (!supervisor_flash_read_block(dest + i * FILESYSTEM_BLOCK_SIZE, block_num + i)) {
            return 1; // error
        }
    }
    return 0; // success
}

mp_uint_t supervisor_flash_write_blocks(const uint8_t *src, uint32_t block_num, uint32_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) {
        if (!supervisor_flash_write_block(src + i * FILESYSTEM_BLOCK_SIZE, block_num + i)) {
            return 1; // error
        }
    }
    return 0; // success
}
