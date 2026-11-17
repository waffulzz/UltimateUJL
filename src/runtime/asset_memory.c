/* Extra assets live in host memory, with stable guest addresses independent of reallocations. */
#include "recomp.h"
#include <stdlib.h>

uint8_t *g_asset_ram;
uint32_t g_asset_ram_capacity;
static uint32_t used;

void rt_asset_reset(void) { used = 0; }
uint32_t rt_asset_mark(void) { return used; }
void rt_asset_rewind(uint32_t mark) { if (mark <= used) used = mark; }

int rt_asset_contains(uint32_t address, size_t size) {
    uint32_t offset = (address & 0x1fffffffu) - HOST_ASSET_BASE;
    return offset < used && size <= used - offset;
}

uint32_t rt_asset_alloc(size_t size) {
    if (!size || size > HOST_ASSET_LIMIT - used) return 0;
    size_t aligned = (size + 15u) & ~(size_t)15u;
    if (aligned > HOST_ASSET_LIMIT - used) return 0;
    uint32_t end = used + (uint32_t)aligned;
    if (end > g_asset_ram_capacity) {
        uint32_t capacity = g_asset_ram_capacity ? g_asset_ram_capacity : 65536;
        while (capacity < end) capacity = capacity > HOST_ASSET_LIMIT / 2 ? HOST_ASSET_LIMIT : capacity * 2;
        uint8_t *next = realloc(g_asset_ram, capacity);
        if (!next) return 0;
        g_asset_ram = next; g_asset_ram_capacity = capacity;
    }
    uint32_t address = 0x80000000u | (HOST_ASSET_BASE + used);
    memset(g_asset_ram + used, 0, aligned);
    used = end;
    return address;
}
