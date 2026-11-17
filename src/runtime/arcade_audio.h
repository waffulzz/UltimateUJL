#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Call before starting the guest thread. No sound data is bundled with the executable. */
bool arcade_audio_init(const char *rom_set);
bool arcade_audio_ready(void);
void arcade_audio_set_enabled(bool enabled);
bool arcade_audio_enabled(void);
void arcade_audio_reset(void);
void arcade_audio_key_on(int voice, const uint8_t *spu_ram, uint32_t address);
int32_t arcade_audio_sample(int voice, uint32_t block_address, uint32_t counter, int32_t original);
#ifdef __cplusplus
}
#endif
