/* Include the source to exercise the PCM reader with synthetic private-state fixtures. */
#include "../src/runtime/arcade_audio.c"
#include <assert.h>
#include <stdio.h>
void host_vblank(void) {}
void host_audio_push(const int16_t *lr, int frames) { (void)lr; (void)frames; }

static void force_crc(uint8_t *bytes, size_t size, uint32_t crc) {
    uint32_t values[32] = {0}, masks[32] = {0}, zero = asset_crc32(0, bytes + size - 4, 4);
    for (unsigned i = 0; i < 32; i++) {
        uint8_t bit[4] = {0}; bit[i / 8] = 1u << (i % 8);
        uint32_t v = asset_crc32(0, bit, 4) ^ zero, m = 1u << i;
        for (int j = 31; j >= 0; j--) if (v & (1u << j)) {
            if (!values[j]) { values[j] = v; masks[j] = m; break; }
            v ^= values[j]; m ^= masks[j];
        }
    }
    uint32_t v = crc ^ asset_crc32(0, bytes, size), mask = 0;
    for (int j = 31; j >= 0; j--) if (v & (1u << j)) { v ^= values[j]; mask ^= masks[j]; }
    assert(!v);
    for (int i = 0; i < 4; i++) bytes[size - 4 + i] = (uint8_t)(mask >> (8 * i));
    assert(asset_crc32(0, bytes, size) == crc);
}
static int check_user_banks(const char *roms, const char *manifest) {
    assert(arcade_audio_init(roms));
    FILE *f = fopen(manifest, "rb");
    assert(f);
    uint8_t *ram = SDL_calloc(524288, 1);
    uint8_t *body = NULL;
    size_t body_size = 0;
    char line[2048], path[1536], previous[1536] = "";
    unsigned offset, size, checked = 0, fallbacks = 0;
    int expected;
    while (fgets(line, sizeof line, f)) {
        assert(sscanf(line, "%1535[^|]|%u|%u|%d", path, &offset, &size, &expected) == 4);
        if (strcmp(path, previous)) {
            SDL_free(body); body = SDL_LoadFile(path, &body_size);
            assert(body); SDL_strlcpy(previous, path, sizeof previous);
        }
        assert(offset <= body_size && size <= body_size - offset && size < 524288 - 0x2000);
        memset(ram, 0, 524288);
        memcpy(ram + 0x2000, body + offset, size);
        arcade_audio_key_on(1, ram, 0x2000);
        if (expected < 0) { assert(!voices[1].map); fallbacks++; }
        else {
            assert(voices[1].map && voices[1].map->wave == expected);
            assert(arcade_audio_sample(1, 0x2000, 20 * 4096, 123456) != 123456);
            checked++;
        }
    }
    fclose(f); SDL_free(body); SDL_free(ram);
    printf("PASS: %u mapped user-bank sources and %u console fallbacks\n", checked, fallbacks);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 3) return check_user_banks(argv[1], argv[2]);
    assert(!arcade_audio_init(NULL) && !arcade_audio_ready());
    assert(decode_pcm(127, 0) == 32512 && decode_pcm(128, 0) == -32768);
    assert(decode_pcm(0, 8) == 0 && decode_pcm(128, 8) == -32);
    assert(decode_pcm(16, 8) == 512 && decode_pcm(24, 8) == 1024);
    assert(decode_pcm(48, 8) == 4096 && decode_pcm(100, 8) == 17408);
    assert(decode_pcm(127, 8) == 31232 && decode_pcm(255, 8) == -31264);
    wave_bytes = SDL_calloc(200000, 1);
    assert(wave_bytes);
    for (unsigned i = 0; i < 100; i++) wave_bytes[i] = i;
    waves[0] = (Wave){0, 100, 0};
    SampleMap test = {0, 1600, 0, -20, 4096};
    voices[1].map = &test; voices[1].start = 0x2000;
    // Native encoder delay remains silent. 63 native source frames become exactly 80 arcade frames.
    assert(arcade_audio_sample(1, 0x2000, 19 * 4096, 99) == 0);
    assert(arcade_audio_sample(1, 0x2000, 20 * 4096, 99) == 0);
    assert(arcade_audio_sample(1, 0x2020, 27 * 4096, 99) == 80 * 256);
    assert(arcade_audio_sample(1, 0x2000, 21 * 4096, 99) == 325);
    assert(arcade_audio_sample(1, 0x2040, 27 * 4096, 99) == 0);
    test.gain = -4096;
    assert(arcade_audio_sample(1, 0x2020, 27 * 4096, 99) == -80 * 256);
    test.gain = 8192;
    assert(arcade_audio_sample(1, 0x2020, 27 * 4096, 99) == 32767);
    arcade_audio_set_enabled(false);
    assert(arcade_audio_sample(1, 0x2020, 27 * 4096, 99) == 99);
    arcade_audio_set_enabled(true);
    assert(arcade_audio_sample(2, 0x2000, 0, 99) == 99);
    assert(arcade_audio_sample(1, 0x3000, 0, 99) == 99);
    assert(arcade_audio_sample(-1, 0x2000, 0, 99) == 99);
    // Hash only playable ADPCM, ending at the first terminal block; ignore VAB padding.
    uint8_t *ram = SDL_calloc(524288, 1);
    assert(ram);
    const SampleMap *known = sample_map;
    ram[0x2000 + known->length - 15] = 1;
    force_crc(ram + 0x2000, known->length, known->crc);
    arcade_audio_key_on(1, ram, 0x2000);
    assert(voices[1].map == known);
    ram[0x2000 + known->length + 4] = 123;
    arcade_audio_key_on(1, ram, 0x2000);
    assert(voices[1].map == known);
    ram[0x2002] ^= 1;
    arcade_audio_key_on(1, ram, 0x2000);
    assert(!voices[1].map && arcade_audio_sample(1, 0x2000, 0, 99) == 99);
    arcade_audio_reset();
    for (int i = 0; i < 24; i++) assert(!voices[i].map);
    for (size_t i = 0; i < sizeof sample_map / sizeof sample_map[0]; i++) {
        assert(sample_map[i].wave < 1820 && sample_map[i].length >= 16);
        assert(find_sample(sample_map[i].crc, sample_map[i].length) == sample_map + i);
        if (i) assert(sample_map[i].crc > sample_map[i - 1].crc ||
            (sample_map[i].crc == sample_map[i - 1].crc && sample_map[i].length > sample_map[i - 1].length));
    }
    SDL_free(ram); SDL_free(wave_bytes); wave_bytes = NULL;
    puts("PASS: arcade PCM rate, timing, gain, key-on lookup and fallback");
    return 0;
}
