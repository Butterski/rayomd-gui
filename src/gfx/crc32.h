/* SPDX-License-Identifier: Apache-2.0 */
/* CRC-32 (polynomial 0xEDB88320, as PNG and zlib's crc32 use), table-driven. Start with 0. */
#ifndef RGUI_GFX_CRC32_H
#define RGUI_GFX_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t rgui_crc32(uint32_t crc, const void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
