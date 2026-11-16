/* Boot: load the executable from the user's disc into RAM and jump into recompiled code. */
#include "psx.h"
#include "gpu.h"
#include "recomp_funcs.h"
#include <stdlib.h>
#include <SDL3/SDL.h>

bool disc_read_file(const char *path, uint8_t **data, uint32_t *size);
void gpu_init(void);
void mdec_init(void);
void spu_init(void);
void patch_init(void);
void disc_enable_patches(int on);

static CPU g_cpu_state;

static bool read_boot_name(char boot[64]) {
    uint8_t *cnf; uint32_t cnf_size;
    if (!disc_read_file("SYSTEM.CNF", &cnf, &cnf_size)) return false;
    memset(boot, 0, 64);
    char *s = strstr((char *)cnf, "BOOT");
    if (s) s = strchr(s, ':');
    if (s) {
        s++;
        while (*s == '\\') s++;
        int n = 0;
        while (*s && *s != ';' && *s != '\r' && *s != '\n' && n < 63) boot[n++] = *s++;
    }
    free(cnf);
    return boot[0] != 0;
}

/* Entry point of the executable this build was recompiled from (the setup wizard's disc check). */
uint32_t rt_recomp_entry(void) { return RECOMP_ENTRY_ADDR; }

/* Does this image boot the executable this build was recompiled from? */
bool rt_probe_disc(const char *path) {
    if (!disc_open(path)) return false;
    char boot[64];
    if (!read_boot_name(boot)) return false;
    uint8_t *exe; uint32_t exe_size;
    if (!disc_read_file(boot, &exe, &exe_size)) return false;
    uint32_t pc = 0;
    if (exe_size >= 0x20 && !memcmp(exe, "PS-X EXE", 8)) memcpy(&pc, exe + 0x10, 4);
    free(exe);
    return pc == RECOMP_ENTRY_ADDR;
}

bool rt_boot(const char *disc_path) {
    if (!disc_open(disc_path)) { rt_log("cannot open disc image: %s", disc_path); return false; }
    uint8_t *cnf; uint32_t cnf_size;
    if (!disc_read_file("SYSTEM.CNF", &cnf, &cnf_size)) { rt_log("SYSTEM.CNF not found"); return false; }
    char boot[64] = {0};
    {
        char *s = strstr((char *)cnf, "BOOT");
        if (!s) return false;
        s = strchr(s, ':');
        if (!s) return false;
        s++;
        while (*s == '\\') s++;
        int n = 0;
        while (*s && *s != ';' && *s != '\r' && *s != '\n' && n < 63) boot[n++] = *s++;
    }
    free(cnf);
    uint8_t *exe; uint32_t exe_size;
    if (!disc_read_file(boot, &exe, &exe_size)) { rt_log("boot exe %s not found", boot); return false; }
    if (memcmp(exe, "PS-X EXE", 8)) { rt_log("%s is not a PS-X EXE", boot); return false; }
    uint32_t pc, gp, base, tsize, sp;
    memcpy(&pc, exe + 0x10, 4);
    memcpy(&gp, exe + 0x14, 4);
    memcpy(&base, exe + 0x18, 4);
    memcpy(&tsize, exe + 0x1C, 4);
    memcpy(&sp, exe + 0x30, 4);
    if (pc != RECOMP_ENTRY_ADDR) {
        rt_log("disc executable %s (entry %08x) does not match this build (entry %08x)", boot, pc, RECOMP_ENTRY_ADDR);
        return false;
    }
    g_ram = (uint8_t *)calloc(PSX_RAM_SIZE, 1);
    memcpy(g_ram + (base & 0x1FFFFF), exe + 0x800, tsize);
    free(exe);
    rt_log("booted %s: %u bytes at %08x, entry %08x", boot, tsize, base, pc);

    CPU *c = &g_cpu_state;
    memset(c, 0, sizeof *c);
    g_cpu = c;
    c->r[28] = gp;
    c->r[29] = c->r[30] = sp ? sp : 0x801FFFF0u;
    c->cop0[12] = 0x40000000u; /* COP2 usable, interrupts off */
    c->budget = 2000;
    patch_init();          /* reads originals, so before enabling */
    disc_enable_patches(1);
    gpu_init();
    mdec_init();
    bios_init(c);
    clock_init();
    spu_init();
    return true;
}

void rt_run(void) {
    RECOMP_ENTRY(g_cpu);
    rt_log("main returned");
}
