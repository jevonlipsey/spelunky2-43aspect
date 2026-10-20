/* spelunky 2 native 4:3 for the android handheld (proton/FEX), shipped as one
 * dll named discord_game_sdk.dll. the game loads it at startup in place of the
 * real discord sdk, which is how it gets into the process.
 *
 * it does three jobs:
 *   - heap fix: patches the allocator's VirtualAlloc hint range so it fits
 *     android's 39-bit address space (patch_heap_limit)
 *   - 4:3: patches the projection, viewport and 2d ui so the game renders
 *     natively at 4:3 instead of letterboxed 16:9
 *   - audio: restores the sfx / music bus volumes the game leaves muted
 *
 * freestanding x86_64 dll with -nostdlib -Wl,-e,DllMain: no CRT, imports only
 * KERNEL32.dll. the web patcher can also embed it in Spel2.exe itself (see
 * embedded_entry).
 */

#include <windows.h>
#include <stdint.h>

#define fabsf(x) __builtin_fabsf(x)

#define MOD_VERSION    "mod_43aspect 4.2"
#define DEFAULT_ASPECT  1.3333334f
#define ORIGINAL_ASPECT 1.7777778f
#define ORIGINAL_INV    0.5625f
#define TOLERANCE       0.005f

#define LOG_NAME "mod_43aspect.log"
#define BOOT_LOG_NAME "mod_43aspect_boot.log"
#define INI_NAME "mod_43aspect.ini"

static int       g_failures = 0;
static uintptr_t g_base     = 0;
static float     g_aspect   = DEFAULT_ASPECT;
static int       g_debug    = 0;
static int       g_embedded = 0; /* 1 = loaded by embedded_entry, not as a dll */

static char g_ini_path[MAX_PATH];

/* ### memory and string intrinsics without CRT */

static void my_memcpy(void* dst, const void* src, size_t n) {
    volatile uint8_t* d = (volatile uint8_t*)dst;
    const volatile uint8_t* s = (const volatile uint8_t*)src;
    while (n--) *d++ = *s++;
}

static int my_memcmp(const void* a, const void* b, size_t n) {
    const volatile uint8_t* s1 = (const volatile uint8_t*)a;
    const volatile uint8_t* s2 = (const volatile uint8_t*)b;
    while (n--) {
        if (*s1 != *s2) return *s1 - *s2;
        s1++; s2++;
    }
    return 0;
}

static size_t my_strlen(const char* s) {
    if (!s) return 0;
    const volatile char* p = (const volatile char*)s;
    size_t len = 0;
    while (*p++) len++;
    return len;
}

static void my_strncpy(char* dst, const char* src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    while (i + 1 < max && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void my_strcat(char* dst, const char* src) {
    if (!dst || !src) return;
    size_t dlen = my_strlen(dst);
    size_t i = 0;
    while (src[i]) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

static char* my_strrchr(const char* s, int c) {
    const volatile char* p = (const volatile char*)s;
    const char* last = NULL;
    while (*p) {
        if (*p == (char)c) last = (const char*)p;
        p++;
    }
    if (c == '\0') return (char*)p;
    return (char*)last;
}

/* format float as string: [-]int.frac */
static void fmt_float(float val, int decimals, char* out, size_t outsz) {
    if (outsz < 2 || !out) return;
    char* p = out;
    if (val < 0.0f) {
        if ((size_t)(p - out + 1) >= outsz) { *out = '\0'; return; }
        *p++ = '-';
        val = -val;
    }
    float round = 0.5f;
    for (int i = 0; i < decimals; i++) round *= 0.1f;
    val += round;

    unsigned long ipart = (unsigned long)val;
    float fpart = val - (float)ipart;

    char temp[24];
    int ti = 0;
    if (ipart == 0) temp[ti++] = '0';
    else {
        while (ipart > 0 && ti < 23) {
            temp[ti++] = (char)('0' + (ipart % 10));
            ipart /= 10;
        }
    }
    while (ti > 0 && (size_t)(p - out + 1) < outsz) *p++ = temp[--ti];

    if (decimals > 0 && (size_t)(p - out + 1) < outsz) {
        *p++ = '.';
        for (int i = 0; i < decimals && (size_t)(p - out + 1) < outsz; i++) {
            fpart *= 10.0f;
            int digit = (int)fpart;
            if (digit > 9) digit = 9;
            if (digit < 0) digit = 0;
            *p++ = (char)('0' + digit);
            fpart -= (float)digit;
        }
    }
    *p = '\0';
}

static void fmt_int(int val, char* out, size_t outsz) {
    if (outsz < 2 || !out) return;
    char* p = out;
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    char temp[16];
    int ti = 0;
    if (val == 0) temp[ti++] = '0';
    else {
        while (val > 0 && ti < 15) {
            temp[ti++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    while (ti > 0 && (size_t)(p - out + 1) < outsz) *p++ = temp[--ti];
    *p = '\0';
}

static void fmt_hex(uint32_t val, char* out, int width, size_t outsz) {
    if (outsz < 2 || !out) return;
    char temp[16];
    int ti = 0;
    if (val == 0 && width <= 1) temp[ti++] = '0';
    else {
        while ((val > 0 || ti < width) && ti < 15) {
            int d = (int)(val & 0xf);
            temp[ti++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
            val >>= 4;
        }
    }
    char* p = out;
    while (ti > 0 && (size_t)(p - out + 1) < outsz) *p++ = temp[--ti];
    *p = '\0';
}

static void fmt_hex64(uint64_t val, char* out, size_t outsz) {
    if (outsz < 2 || !out) return;
    char temp[20];
    int ti = 0;
    if (val == 0) temp[ti++] = '0';
    else {
        while (val > 0 && ti < 19) {
            int d = (int)(val & 0xf);
            temp[ti++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
            val >>= 4;
        }
    }
    char* p = out;
    while (ti > 0 && (size_t)(p - out + 1) < outsz) *p++ = temp[--ti];
    *p = '\0';
}

/* mini formatting helper replacing wsprintfA */
static void my_format(char* out, size_t outsz, const char* fmt, ...) {
    if (!out || outsz == 0) return;
    out[0] = '\0';
    const char* f = fmt;
    char* d = out;
    size_t rem = outsz - 1;

    va_list args;
    va_start(args, fmt);

    while (*f && rem > 0) {
        if (*f == '%' && *(f + 1)) {
            f++;
            if (*f == 's') {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s && rem > 0) { *d++ = *s++; rem--; }
            } else if (*f == 'd') {
                int v = va_arg(args, int);
                char ibuf[16];
                fmt_int(v, ibuf, sizeof(ibuf));
                const char* s = ibuf;
                while (*s && rem > 0) { *d++ = *s++; rem--; }
            } else if (*f == 'x') {
                uint32_t v = va_arg(args, uint32_t);
                char xbuf[16];
                fmt_hex(v, xbuf, 0, sizeof(xbuf));
                const char* s = xbuf;
                while (*s && rem > 0) { *d++ = *s++; rem--; }
            } else if (*f == '0' && *(f + 1) == '2' && *(f + 2) == 'x') {
                f += 2;
                uint32_t v = va_arg(args, uint32_t);
                char xbuf[16];
                fmt_hex(v, xbuf, 2, sizeof(xbuf));
                const char* s = xbuf;
                while (*s && rem > 0) { *d++ = *s++; rem--; }
            } else if (*f == '%') {
                *d++ = '%'; rem--;
            }
            f++;
        } else {
            *d++ = *f++;
            rem--;
        }
    }
    *d = '\0';
    va_end(args);
}

/* ### aspect globals and projection offsets */

#define RVA_ASPECT_MAIN 0x22d0c4a0u
#define RVA_ASPECT_INV  0x22d0d620u
#define RVA_ASPECT_XMM  0x22d0e440u

#define RVA_PROJ_INV_NEG 0x22d0d624u
#define RVA_PROJ_10134   0x22d10134u
#define RVA_PROJ_1013C   0x22d1013cu
#define RVA_PROJ_10148   0x22d10148u
#define RVA_PROJ_10160   0x22d10160u
#define RVA_PROJ_10180   0x22d10180u
#define RVA_PROJ_10184   0x22d10184u
#define RVA_PROJ_10188   0x22d10188u

#define RVA_PROJ_M11     0x22d12424u
#define RVA_PROJ_M00     0x22d12410u
#define RVA_CAM_Y_SCALE  0x22d0f844u

#define RVA_CAM_EXT_F840  0x22d0f840u
#define RVA_CAM_EXT_F854  0x22d0f854u
#define RVA_CAM_EXT_F870  0x22d0f870u
#define RVA_CAM_EXT_F87C  0x22d0f87cu

#define RVA_LETTERBOX_JBE 0x22cc197fu

#define RVA_ZOOM_DEFAULT 0x22d0d120u
#define RVA_ZOOM_SHOP    0x22d0e904u
#define RVA_ZOOM_CODE1   0x229e7726u
#define RVA_ZOOM_CODE2   0x22c91b93u

static void get_exe_dir_file(char* out, size_t sz, const char* filename) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) { my_strncpy(out, filename, sz); return; }
    path[MAX_PATH - 1] = '\0';
    char* slash = my_strrchr(path, '\\');
    if (!slash) slash = my_strrchr(path, '/');
    if (slash) slash[1] = '\0'; else path[0] = '\0';
    my_strncpy(out, path, sz);
    my_strcat(out, filename);
}

/* resolved once: log_raw runs on the render thread, and a module-path query
 * plus an open/close per line made the first pause (~170 lines) hitch. */
static char g_log_path[MAX_PATH];
static char g_boot_path[MAX_PATH];
static int  g_paths_ready = 0;

static void init_log_paths(void) {
    if (g_paths_ready) return;
    get_exe_dir_file(g_log_path, sizeof(g_log_path), LOG_NAME);
    get_exe_dir_file(g_boot_path, sizeof(g_boot_path), BOOT_LOG_NAME);
    g_paths_ready = 1;
}

static void append_line(const char* path, const char* text, DWORD len) {
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, text, len, &wrote, NULL);
    WriteFile(h, "\r\n", 2, &wrote, NULL);
    CloseHandle(h);
}

static void truncate_file(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void crumb(const char* s) {
    init_log_paths();
    append_line(g_boot_path, s, (DWORD)my_strlen(s));
}

static void log_raw(const char* s) {
    if (!g_debug) return;
    init_log_paths();
    append_line(g_log_path, s, (DWORD)my_strlen(s));
}

static void boot_raw(const char* s) {
    crumb(s);
}

static int safe_read_float(uintptr_t addr, float* out) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    my_memcpy(out, (const void*)addr, sizeof(float));
    return 1;
}

static void write_float(uintptr_t addr, float v) {
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)addr, sizeof(float), PAGE_EXECUTE_READWRITE, &old)) return;
    my_memcpy((void*)addr, &v, sizeof(float));
    DWORD ignored;
    VirtualProtect((LPVOID)addr, sizeof(float), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, sizeof(float));
}

static int patch_float(uintptr_t addr, float expect_lo, float expect_hi,
                       float new_val, const char* name) {
    float cur = 0.0f;
    if (!safe_read_float(addr, &cur)) {
        char buf[128];
        my_format(buf, sizeof(buf), "[FAIL] %s: unreadable", name);
        log_raw(buf);
        g_failures++;
        return 0;
    }
    float lo = expect_lo < expect_hi ? expect_lo : expect_hi;
    float hi = expect_lo < expect_hi ? expect_hi : expect_lo;
    if (fabsf(cur - lo) > TOLERANCE && fabsf(cur - hi) > TOLERANCE) {
        char buf[128], f1[32];
        fmt_float(cur, 5, f1, sizeof(f1));
        my_format(buf, sizeof(buf), "[....] %s: read %s, not expected", name, f1);
        log_raw(buf);
        g_failures++;
        return 0;
    }
    if (cur == new_val) {
        char buf[128], f1[32];
        fmt_float(new_val, 5, f1, sizeof(f1));
        my_format(buf, sizeof(buf), "[ OK ] %s: already %s", name, f1);
        log_raw(buf);
        return 1;
    }
    write_float(addr, new_val);
    float chk = 0.0f;
    safe_read_float(addr, &chk);
    if (chk == new_val) {
        char buf[128], f1[32], f2[32];
        fmt_float(cur, 5, f1, sizeof(f1));
        fmt_float(new_val, 5, f2, sizeof(f2));
        my_format(buf, sizeof(buf), "[ OK ] %s: %s -> %s (verified)", name, f1, f2);
        log_raw(buf);
        return 1;
    }
    char buf[128], f1[32], f2[32];
    fmt_float(chk, 5, f1, sizeof(f1));
    fmt_float(new_val, 5, f2, sizeof(f2));
    my_format(buf, sizeof(buf), "[FAIL] %s: readback %s != %s", name, f1, f2);
    log_raw(buf);
    g_failures++;
    return 0;
}

static int patch_bytes(uintptr_t addr, const unsigned char* expect,
                       const unsigned char* repl, size_t n, const char* name) {
    unsigned char cur[32], chk[32];
    if (n > sizeof(cur)) return 0;
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        char buf[128];
        my_format(buf, sizeof(buf), "[FAIL] %s: VirtualProtect failed", name);
        log_raw(buf);
        g_failures++;
        return 0;
    }
    my_memcpy(cur, (void*)addr, n);
    if (my_memcmp(cur, expect, n) == 0) {
        my_memcpy((void*)addr, repl, n);
    } else if (my_memcmp(cur, repl, n) != 0) {
        char buf[128];
        my_format(buf, sizeof(buf), "[FAIL] %s: unexpected bytes %02x%02x", name, (uint32_t)cur[0], (uint32_t)cur[1]);
        log_raw(buf);
        VirtualProtect((LPVOID)addr, n, old, &old);
        g_failures++;
        return 0;
    }
    DWORD ignored;
    VirtualProtect((LPVOID)addr, n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, n);
    my_memcpy(chk, (void*)addr, n);
    if (my_memcmp(chk, repl, n) != 0) {
        char buf[128];
        my_format(buf, sizeof(buf), "[FAIL] %s: readback mismatch", name);
        log_raw(buf);
        g_failures++;
        return 0;
    }
    char buf[128];
    if (n > 1)
        my_format(buf, sizeof(buf), "[ OK ] %s: %02x%02x -> %02x%02x (patched)", name,
                  (uint32_t)expect[0], (uint32_t)expect[1], (uint32_t)repl[0], (uint32_t)repl[1]);
    else
        my_format(buf, sizeof(buf), "[ OK ] %s: %02x -> %02x (patched)", name,
                  (uint32_t)expect[0], (uint32_t)repl[0]);
    log_raw(buf);
    return 1;
}

/* ### heap compatibility
 *
 * the game VirtualAllocs 5 x 32 MiB blocks at a random hint of
 * (0x701..0x7FF << 32) | 0x7F800000 and re-rolls forever when the allocation
 * fails. android's 39-bit user VA (512 GiB, top hint 0x7F << 32) can never
 * satisfy that, so the game hangs at boot. two one-byte patches keep the hint
 * at (0x01..0x7F << 32): the movabs high dword 0x701 -> 0x001, and the mask
 * 0xFE -> 0x7E on the random byte. both sit in one function. */

#define HEAP_FN_LO 0x22CB8E30u
#define HEAP_FN_HI 0x22CC21E9u

static const struct heap_site {
    uintptr_t rva;               /* rva of sig[0] */
    unsigned char sig[16];       /* stock bytes; unique in the exe */
    int len;
    int po;                      /* offset of the patched byte in sig */
    unsigned char to;            /* patched value */
    const char* name;
} g_heap_sites[2] = {
    { 0x22CBD624u, { 0x48, 0xBB, 0x00, 0x00, 0x80, 0x7F, 0x01, 0x07, 0x00, 0x00, 0x48, 0x8B, 0x35 },
      13, 7, 0x00, "heap a (hint high dword)" },
    { 0x22CBD6B8u, { 0x31, 0xC1, 0x81, 0xE1, 0xFE, 0x00, 0x00, 0x00, 0x48, 0xC1, 0xE1, 0x20, 0x48, 0x09, 0xD9 },
      15, 4, 0x7E, "heap b (hint mask)" },
};

/* is [lo, hi) committed and readable? the range is .text, so one pass suffices */
static int range_readable(uintptr_t lo, uintptr_t hi) {
    uintptr_t cur = lo;
    while (cur < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= cur) return 0;
        cur = next;
    }
    return 1;
}

/* 0 = not found / ambiguous, 1 = stock at *at, 2 = already patched at *at */
static int heap_locate(const struct heap_site* st, uintptr_t* at) {
    unsigned char stock[16], done[16];
    my_memcpy(stock, st->sig, (size_t)st->len);
    my_memcpy(done, st->sig, (size_t)st->len);
    done[st->po] = st->to;

    uintptr_t lo = g_base + HEAP_FN_LO, hi = g_base + HEAP_FN_HI;
    uintptr_t exact = g_base + st->rva;
    if (range_readable(exact, exact + (uintptr_t)st->len)) {
        if (my_memcmp((const void*)exact, stock, (size_t)st->len) == 0) { *at = exact; return 1; }
        if (my_memcmp((const void*)exact, done, (size_t)st->len) == 0) { *at = exact; return 2; }
    }
    if (!range_readable(lo, hi)) return 0;
    int found = 0, kind = 0;
    uintptr_t where = 0;
    for (uintptr_t a = lo; a + (uintptr_t)st->len <= hi; a++) {
        const unsigned char* c = (const unsigned char*)a;
        if (c[0] != stock[0]) continue;
        int k = my_memcmp(c, stock, (size_t)st->len) == 0 ? 1
              : my_memcmp(c, done, (size_t)st->len) == 0 ? 2 : 0;
        if (!k) continue;
        found++;
        kind = k;
        where = a;
    }
    if (found != 1) return 0;
    *at = where;
    return kind;
}

static void patch_heap_limit(void) {
    uintptr_t at[2];
    int kind[2];
    char buf[96];
    for (int i = 0; i < 2; i++) {
        kind[i] = heap_locate(&g_heap_sites[i], &at[i]);
        if (!kind[i]) {
            my_format(buf, sizeof(buf), "[FAIL] %s: not found (or ambiguous), heap left alone", g_heap_sites[i].name);
            log_raw(buf);
            g_failures++;
            return;
        }
    }
    for (int i = 0; i < 2; i++) {
        const struct heap_site* st = &g_heap_sites[i];
        if (kind[i] == 2) {
            my_format(buf, sizeof(buf), "[ OK ] heap %s already patched", i ? "b" : "a");
            log_raw(buf);
            continue;
        }
        unsigned char to = st->to;
        patch_bytes(at[i] + (uintptr_t)st->po, &st->sig[st->po], &to, 1, st->name);
    }
}

static const float ASPECT_DERIVED_S0[] = {
    0.0625f, 0.0400f, 0.3250f, 0.1250f, 0.05859375f, 0.0500f, 0.0675f
};

static int s0_is_aspect_derived(float v) {
    for (size_t i = 0; i < sizeof(ASPECT_DERIVED_S0) / sizeof(ASPECT_DERIVED_S0[0]); i++)
        if (fabsf(fabsf(v) - ASPECT_DERIVED_S0[i]) < 0.002f) return 1;
    return 0;
}

static int patch_vec4(uintptr_t addr, float target_aspect, const char* name) {
    float v[4];
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)addr, 16, PAGE_EXECUTE_READWRITE, &old)) {
        char buf[128];
        my_format(buf, sizeof(buf), "[FAIL] %s: VirtualProtect failed", name);
        log_raw(buf);
        g_failures++;
        return 0;
    }
    my_memcpy(v, (void*)addr, 16);

    int hits = 0;
    if (fabsf(v[0] - ORIGINAL_ASPECT) < TOLERANCE) {
        v[0] = target_aspect;
        hits++;
    } else if (v[0] != 0.0f) {
        if (s0_is_aspect_derived(v[0])) {
            float s0_old = v[0];
            v[0] = s0_old * ((1.0f / target_aspect) / ORIGINAL_INV);
            hits++;
        }
    }

    for (int i = 1; i < 4; i++) {
        if (fabsf(v[i] - ORIGINAL_ASPECT) < TOLERANCE) {
            v[i] = target_aspect;
            hits++;
        }
    }

    if (!hits) { VirtualProtect((LPVOID)addr, 16, old, &old); return 0; }

    my_memcpy((void*)addr, v, 16);
    DWORD ignored;
    VirtualProtect((LPVOID)addr, 16, old, &ignored);
    float chk[4];
    my_memcpy(chk, (void*)addr, 16);
    if (my_memcmp(chk, v, 16) != 0) {
        char buf[128];
        my_format(buf, sizeof(buf), "[FAIL] %s: readback mismatch", name);
        log_raw(buf);
        g_failures++;
        return 0;
    }
    char buf[128];
    my_format(buf, sizeof(buf), "[ OK ] %s patched", name);
    log_raw(buf);
    return 1;
}

#define IMM_16_9 0x3FE38E39u
#define IMM_4_3  0x3FAAAAABu

static const uint32_t RUNTIME_ASPECT_SITES[] = {
    0x22b6699au, 0x22b97d13u, 0x22b9849eu, 0x22b98876u, 0x22b989beu, 0x22b9bdf9u,
    0x22b9c50fu, 0x22b9c95bu, 0x22b9f5c3u, 0x22ba1d45u, 0x22ba1fafu, 0x22ba4e0cu,
    0x22ba5936u,
};

static void patch_runtime_aspect(uintptr_t base, float target_aspect) {
    uint32_t new_imm;
    int i, n = (int)(sizeof(RUNTIME_ASPECT_SITES) / sizeof(RUNTIME_ASPECT_SITES[0]));
    int hit = 0, skipped = 0;

    my_memcpy(&new_imm, &target_aspect, sizeof(new_imm));
    if (new_imm != IMM_4_3 && fabsf(target_aspect - DEFAULT_ASPECT) > TOLERANCE) {
        log_raw("[skip] runtime immediates only know 4:3 pattern");
        return;
    }

    for (i = 0; i < n; i++) {
        uintptr_t a = base + RUNTIME_ASPECT_SITES[i];
        DWORD old = 0;
        uint32_t cur = 0, chk = 0;

        if (!VirtualProtect((LPVOID)a, 4, PAGE_EXECUTE_READWRITE, &old)) {
            g_failures++;
            continue;
        }
        my_memcpy(&cur, (void*)a, 4);
        if (cur != IMM_16_9 && cur != new_imm) {
            VirtualProtect((LPVOID)a, 4, old, &old);
            skipped++;
            continue;
        }
        my_memcpy((void*)a, &new_imm, 4);
        VirtualProtect((LPVOID)a, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)a, 4);
        my_memcpy(&chk, (void*)a, 4);
        if (chk == new_imm) { hit++; }
        else { g_failures++; }
    }
    char buf[128];
    my_format(buf, sizeof(buf), "[*] runtime 16:9 immediates: %d/%d patched, %d skipped", hit, n, skipped);
    log_raw(buf);
}

#define MAX_LIVE_M00 16
static uintptr_t g_live_m00_ptrs[MAX_LIVE_M00];
static int       g_live_m00_count = 0;

/* off by default: it has never patched a single matrix in any logged run, and
 * it is what killed pin10 (and, most likely, 403f705) - a region the game or
 * d3d frees between VirtualQuery and the read faults the scan thread
 * (mod+0x2c10, read of a page boundary, run 11). */
static int g_do_live_scan = 0;
static int g_do_heap_patch = 1;
static int g_do_force_audio = 1;

/* the scan reads through ReadProcessMemory in chunks, which fails cleanly on a
 * region that went away, instead of dereferencing it. 60 bytes of overlap so a
 * matrix straddling two chunks is still seen whole. */
#define LIVE_SCAN_CHUNK 0x10000
static uint8_t g_live_scan_buf[LIVE_SCAN_CHUNK + 64];

static void scan_and_patch_live_matrices(float target_m00, float target_m11) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);

    uintptr_t min_addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t max_addr = (uintptr_t)si.lpMaximumApplicationAddress;
    uintptr_t cur_addr = min_addr;
    HANDLE self = GetCurrentProcess();

    int patched_count = 0;
    MEMORY_BASIC_INFORMATION mbi;

    while (cur_addr < max_addr) {
        if (!VirtualQuery((LPCVOID)cur_addr, &mbi, sizeof(mbi))) break;

        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {

            uint8_t* p = (uint8_t*)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;

            // search for stock m00 (1.3579951) aligned by 4 bytes
            for (size_t base = 0; base + 64 <= sz; base += LIVE_SCAN_CHUNK) {
              SIZE_T want = sz - base, got = 0;
              if (want > LIVE_SCAN_CHUNK + 60) want = LIVE_SCAN_CHUNK + 60;
              if (!ReadProcessMemory(self, p + base, g_live_scan_buf, want, &got)) break;
              for (size_t off = 0; off + 64 <= got && off < LIVE_SCAN_CHUNK; off += 4) {
                float* f = (float*)(g_live_scan_buf + off);
                if (fabsf(f[0] - 1.3579951f) < 0.001f) {
                    // candidate float f[0] is ONLY treated as 4x4 proj matrix if:
                    // m11 at +0x14 bytes (f[5]) is 2.4142136f,
                    // and off-diagonal elements (f[1], f[2], f[3], f[4]) are 0.0f
                    if (fabsf(f[5] - 2.4142136f) < 0.001f &&
                        fabsf(f[1]) < 0.0001f &&
                        fabsf(f[2]) < 0.0001f &&
                        fabsf(f[3]) < 0.0001f &&
                        fabsf(f[4]) < 0.0001f) {

                        uintptr_t m00_addr = (uintptr_t)(p + base + off);
                        if (!WriteProcessMemory(self, (LPVOID)m00_addr, &target_m00, sizeof(float), NULL))
                            continue;
                        f[0] = target_m00;
                        patched_count++;

                        // Cache address for high-frequency watchdog locking
                        int known = 0;
                        for (int k = 0; k < g_live_m00_count; k++) {
                            if (g_live_m00_ptrs[k] == m00_addr) { known = 1; break; }
                        }
                        if (!known && g_live_m00_count < MAX_LIVE_M00) {
                            g_live_m00_ptrs[g_live_m00_count++] = m00_addr;
                        }

                        char buf[256], hex_addr[32];
                        fmt_hex64((uint64_t)m00_addr, hex_addr, sizeof(hex_addr));

                        // Dump surrounding 16 floats (4x4 matrix) on first match for diagnostic
                        if (patched_count <= 5) {
                            float surrounding[16];
                            my_memcpy(surrounding, f, sizeof(surrounding));
                            char s0[16], s1[16], s5[16], s10[16];
                            fmt_float(surrounding[0], 4, s0, sizeof(s0));
                            fmt_float(surrounding[1], 4, s1, sizeof(s1));
                            fmt_float(surrounding[5], 4, s5, sizeof(s5));
                            fmt_float(surrounding[10], 4, s10, sizeof(s10));
                            my_format(buf, sizeof(buf), "[live] heap m00 at 0x%s patched (m00=%s m01=%s m11=%s m22=%s)",
                                      hex_addr, s0, s1, s5, s10);
                        } else {
                            my_format(buf, sizeof(buf), "[live] patched heap m00 at 0x%s to 4:3 (total %d)", hex_addr, patched_count);
                        }
                        log_raw(buf);
                    }
                }
              }
            }
        }

        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= cur_addr) break;
        cur_addr = next;
    }
}

static DWORD WINAPI matrix_report_thread(LPVOID unused) {
    (void)unused;
    for (int i = 0; i < 12; i++) {
        Sleep(2500);
        float m00 = 0.0f, m11 = 0.0f, zoom = 0.0f, ys = 0.0f;
        if (!safe_read_float(g_base + RVA_PROJ_M00, &m00) ||
            !safe_read_float(g_base + RVA_PROJ_M11, &m11)) {
            log_raw("[matrix] projection constants unreadable");
            continue;
        }
        safe_read_float(g_base + RVA_ZOOM_DEFAULT, &zoom);
        safe_read_float(g_base + RVA_CAM_Y_SCALE, &ys);

        // Also run live heap scanner to patch any copies Spel2.exe made to runtime memory
        float want00 = g_aspect != 0.0f ? 2.4142136f / g_aspect : 0.0f;
        if (want00 > 0.0f && g_do_live_scan) {
            scan_and_patch_live_matrices(want00, 2.4142136f);
        }

        float vhalf = zoom * ys;
        float ux = m00 != 0.0f ? 1.0f / m00 : 0.0f;
        float uy = m11 != 0.0f ? 1.0f / m11 : 0.0f;
        float upt = vhalf != 0.0f ? uy / vhalf : 0.0f;
        float hhalf = upt != 0.0f ? ux / upt : 0.0f;
        float ar = vhalf != 0.0f ? hhalf / vhalf : 0.0f;

        char buf[256], sm00[32], sm11[32], sh[32], sv[32], sar[32];
        fmt_float(m00, 5, sm00, sizeof(sm00));
        fmt_float(m11, 5, sm11, sizeof(sm11));
        fmt_float(hhalf * 2.0f, 3, sh, sizeof(sh));
        fmt_float(vhalf * 2.0f, 3, sv, sizeof(sv));
        fmt_float(ar, 4, sar, sizeof(sar));

        my_format(buf, sizeof(buf), "[matrix] poll %d: m00=%s m11=%s -> full %s x %s tiles  ar=%s%s",
                  i, sm00, sm11, sh, sv, sar,
                  (ar > 1.328f && ar < 1.339f) ? "   [ 4:3 OK ]" : "");
        log_raw(buf);
    }
    return 0;
}

static DWORD WINAPI watchdog_thread(LPVOID unused) {
    (void)unused;
    float want00 = g_aspect != 0.0f ? 2.4142136f / g_aspect : 0.0f;
    for (int tick = 0; tick < 1800; tick++) { // 90 seconds @ 50ms intervals
        Sleep(50);
        float m00 = 0.0f, am = 0.0f;
        if (safe_read_float(g_base + RVA_PROJ_M00, &m00)) {
            safe_read_float(g_base + RVA_ASPECT_MAIN, &am);
            if (want00 > 0.0f && fabsf(m00 - want00) > TOLERANCE) {
                write_float(g_base + RVA_PROJ_M00, want00);
            }
        }
        // High-frequency lock on all dynamic constant buffer heap pointers
        if (want00 > 0.0f && g_live_m00_count > 0) {
            for (int k = 0; k < g_live_m00_count; k++) {
                uintptr_t ptr = g_live_m00_ptrs[k];
                float cur = 0.0f;
                if (safe_read_float(ptr, &cur) && fabsf(cur - want00) > TOLERANCE) {
                    /* heap copies can be freed under us; a failed write is fine */
                    WriteProcessMemory(GetCurrentProcess(), (LPVOID)ptr, &want00, sizeof(float), NULL);
                }
            }
        }
    }
    log_raw("[watch] 90s clean, dynamic m00 maintained");
    return 0;
}

static int   g_do_world = 1, g_do_noletterbox = 1, g_do_camext = 1;
static int   g_do_aspect_globals = 1, g_do_proj_constants = 0, g_do_aspect_vectors = 0, g_do_runtime_immediates = 0;
static int   g_do_ui_quads = 0;
static int   g_do_hud_fix = 0;
static float g_ui_scale_x = 1.0f;
static float g_ui_scale_y = 1.0f;
static float g_hud_x_scale = 1.0f;
/* 13.5 is vanilla zoom. do not raise it: the camera's vertical view is
 * zoom*0.2667*2 tiles, so 15.6 is 8.32 tiles tall - taller than the ~8 tile
 * room - which leaves the vertical clamp with no valid range and makes the
 * camera jut violently on every level entry / rope descend. */
static float g_custom_zoom = 13.5f;
static int   g_do_ui_letterbox = 0;

/* ### 2d ui layer

 * every screen-space 2d element - hud icons, hearts/bombs/ropes counters,
 * the timer, menu and pause glyphs, the loading spinner - is appended to one
 * pooled vertex batch and drawn by exactly one function:
 *
 *   FUN_162cc5d80   (rva 0x22cc5d80)    69 direct callers in .text
 *
 * so that call is the single choke point for the whole 2d layer. the pending
 * batch lives on the display object (all of this read out of the code):
 *
 *   +0x802f0  vertex buffer wrapper    +0x802f8  mapped write pointer
 *   +0x80300  vertex room left         +0x80308  index buffer wrapper
 *   +0x80310  index write pointer      +0x80318  pending index count
 *   +0x8031c  pending vertex count     +0x802ad  render state byte
 *
 * the flush draws [+0x80318] indices, then releases both wrappers and zeroes
 * +0x802f0..+0x802ff and +0x80308..+0x8031f, so +0x8031c is always the vertex
 * count of *this* batch and the batch is exactly
 *
 *   [ *(u64*)(disp+0x802f8) - n*stride , *(u64*)(disp+0x802f8) )
 *
 * stride 0x18 - the emitters write x,y,z,u,v,argb and z is always 0.
 *
 * the elements are authored against a 16:9 frame, but the canvas is now the
 * whole 4:3 window, so all of them come out 960/720 = 1.3333 too tall: hud
 * hearts look stretched and menu text is oversized. scaling y by
 * 720/960 = 0.75 is what drawing inside a centred 16:9 band does, so content
 * lands back on square pixels. the scale anchor is chosen per element: hud in
 * the corners keeps its distance to the edge it hugs, everything else scales
 * about its own y centre so no authored position moves, and an element that
 * covers the frame - a full screen fade / dim / blur - is left at full height
 * so it keeps covering the window. what counts as one element is
 * ui_2d_cluster / ui_2d_rowgap / ui_2d_pinx, see the decision code below.
 *
 * two things in the same flush must NOT be scaled, and both have bitten:
 *   - the world's 2d geometry, y in the tens (see UI_2D_NDC_Y);
 *   - the full screen fills (fade, dim, blur) - skipping them per vertex
 *     instead of per element is what left a 16:9 slice on screen.
 *
 * keep ui_2d and ui_letterbox mutually exclusive - both fix the same axis
 * from opposite ends and would double up. */

#define RVA_UI_DISP_CACHE 0x22e18c68u
#define RVA_UI_FLUSH      0x22cc5d80u
#define RVA_UI_FLUSH_BODY 0x22cc5d8eu
#define RVA_UI_DRAW_HOOK  0x22c5e12du

/* the same flush carries the world's 2d geometry, with y in the tens (the
 * dump's own filter uses this bound): no 2d element is ever this far out, so
 * anything beyond +-UI_2D_NDC_Y is not ours to scale. scaling the world per
 * cluster squashed every room row about its own centre and left black bands
 * across the level - the run the guest rejected. */
#define UI_2D_NDC_Y 8.0f

/* a fade / dim / blur fill is there to cover the frame; one that is this tall
 * counts as a fill however it is authored. it used to have to touch an edge
 * as well, which let the main menu's tunnel (x +-0.533, y +-0.948, behind a
 * +-1 cave fill) shrink to +-0.711 and open black bars inside the cave. */
#define UI_2D_FILL_SPAN 1.8f

/* a quad with the frame's own proportions in ndc (as wide as it is tall) is a
 * full-frame image: the camp / menu scene zooming in or out through a
 * composite quad (+-0.62 -> +-2.9). it is the same picture the frame shows at
 * rest, unscaled, so it stays unscaled at every size - scaling it until it
 * reached +-1 is the 16:9 pane that popped to 4:3 at the end of each zoom. */
#define UI_2D_FRAME_MIN 0.5f
#define UI_2D_FRAME_TOL 0.02f

/* a strip of this many tiles, this narrow, is the camp's centre rope: it
 * belongs to the zooming scene, so it is unscaled with it (at rest it spans
 * +-1.001 and is a fill anyway). */
#define UI_2D_ROPE_N 4
#define UI_2D_ROPE_W 0.15f

/* the row split (ui_2d_rowgap) is only allowed to cut glyph-sized quads that
 * follow a wide run - a line of text. art pieces are wider than UI_2D_ROW_W
 * and a rope strip is narrower than UI_2D_ROW_RUN, so neither is ever cut, and
 * a run shorter than UI_2D_ROW_N quads is never a line either. 4, not 6: the
 * pause title is four letters, so with 6 the run never qualified and the title
 * stayed welded to the first menu row, leaving both off their boards. */
#define UI_2D_ROW_H   0.12f
#define UI_2D_ROW_W   0.06f
#define UI_2D_ROW_RUN 0.06f
#define UI_2D_ROW_N   4

/* ui_2d_bars: two equal-height pieces whose centres are this far apart (but
 * less than 0.75 of their height) are two levels of backing bars, not one
 * element. off by default, see ui_2d_cluster. */
#define UI_2D_BAR_D   0.05f

/* a vertical strip continues when the next quad has the same x box as the run
 * and touches it in y. a rope drawn in one batch has a pitch above
 * ui_2d_cluster in its last frames (shape 272: 9 quads, 0.223 apart), and the
 * centre rule then cut it between every segment. */
#define UI_2D_STRIP_X   0.005f
#define UI_2D_STRIP_GAP 0.005f

/* ui_2d_center: a cluster whose x centre is within this of the screen centre
 * is part of a centred overlay (pause board, confirm paper). only for clusters
 * inside the frame: the camp fog is wider than the screen and drifts, its x
 * centre crossed 0.02 every few seconds (0.013 <-> 0.021, run 12) and the
 * anchor flipped 0 <-> -1, so the clouds jumped ~120 px. */
#define UI_2D_CENTER_X 0.02f

/* written by the patch functions, read by the trampoline below */
volatile uintptr_t ui_flush_body_va = 0ul;

/* replays the 14 prologue bytes of FUN_162cc5d80 (5 pushes, sub rsp,0x60,
 * mov rsi,rcx) and jumps to the body at 0x22cc5d8e, so the game's own
 * epilogue and every one of its return paths still run. call it with
 * rcx = display, exactly like the original function. */
void ui_flush_body_enter(void* disp);

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl ui_flush_body_enter\n"
    "ui_flush_body_enter:\n"
    "    push %r15\n"
    "    push %r14\n"
    "    push %rsi\n"
    "    push %rdi\n"
    "    push %rbx\n"
    "    sub  $0x60, %rsp\n"
    "    mov  %rcx, %rsi\n"
    "    mov  ui_flush_body_va(%rip), %rax\n"
    "    jmp  *%rax\n"
);

static int   g_do_ui_2d      = 1;
static float g_ui_2d_scale_x = 1.0f;
static float g_ui_2d_edge    = 0.995f;
static float g_ui_2d_pin     = 0.85f;
static float g_ui_2d_pinspan = 0.35f;
static float g_ui_2d_cluster = 0.20f;
static float g_ui_2d_pinx    = 0.30f;
static float g_ui_2d_rowgap  = 0.05f;
static int   g_ui_2d_fill    = 1;
static int   g_ui_2d_bars    = 0;
static int   g_ui_2d_center  = 1;
static int   g_do_ui_2d_dump = 0;
static int   g_ui_2d_batches = 0;
static int   g_ui_2d_verts   = 0;

/* 0x18 for the normal xyz/uv/argb stream; the odd 0x24 variant is picked by
 * the input layout when this byte is set, and we leave those alone. */
static int ui_2d_stride(uint8_t* disp) {
    uint8_t state = *(uint8_t*)(disp + 0x802ad);
    uint8_t idx   = *(uint8_t*)(disp + (uintptr_t)state * 0x30 + 0x80560);
    return *(uint8_t*)(disp + (uintptr_t)idx + 0x804f8) ? 0x24 : 0x18;
}

/* the batch is a mapped vertex buffer, so these are ordinary aligned floats;
 * may_alias keeps -fno-builtin from turning the loads into byte loops */
typedef float __attribute__((may_alias)) ui_2d_f32;

/* how many vertices one element takes in the batch being walked. an element is
 * a quad, and a quad ships either as 4 indexed vertices (idx = 1.5n) or as the
 * two triangles it is made of (idx = n, 6 vertices). grouping 4 at a time was
 * right for the first and wrong for the second: the groups then straddle the
 * element boundaries, a group holding the tail of one line and the head of the
 * next has a box that bridges the gap between them, and the row split that
 * keeps menu text on its bars could never fire. n * 6 / idx is the vertices per
 * quad either way. */
static int   g_ui_2d_vpq     = 4;

/* a rope is a column of one-quad batches. scaling each about its own centre
 * opens the gaps the artist left between them into bands, so a narrow quad
 * that sits under the previous narrow quad of the same column reuses that
 * quad's anchor and the whole column scales as one piece. the x match is tight
 * (the segments of a rope share their exact column) and the y has to come
 * after, so hud digits - same shape, no column - keep their own anchors.
 * 12 slots: the camp draws both banner ropes alternating with each other, and
 * a rope's segment is only a few batches away from the one above it - with 4
 * slots the column was evicted before its next segment arrived. */
#define UI_2D_CHAIN_N 12
#define UI_2D_CHAIN_X 0.005f
#define UI_2D_CHAIN_Y 0.30f
static float g_ui_2d_chx0[UI_2D_CHAIN_N], g_ui_2d_chx1[UI_2D_CHAIN_N];
static float g_ui_2d_chcy[UI_2D_CHAIN_N], g_ui_2d_chanchor[UI_2D_CHAIN_N];
static int   g_ui_2d_chused[UI_2D_CHAIN_N];
static int   g_ui_2d_chnext = 0;

static float ui_2d_ldf(const uint8_t* p) {
    return *(const ui_2d_f32*)p;
}

static void ui_2d_stf(uint8_t* p, float v) {
    *(ui_2d_f32*)p = v;
}

static float ui_2d_absf(float v) {
    return v < 0.0f ? -v : v;
}

/* mean y of the vertices of group g. the tail group of a batch whose count is
 * not a multiple of the group size takes what is left. */
static float ui_2d_group_cy(uint8_t* v0, int n, int g) {
    int base = g * g_ui_2d_vpq, cnt = n - base, k;
    float s = 0.0f;

    if (cnt > g_ui_2d_vpq) cnt = g_ui_2d_vpq;
    for (k = 0; k < cnt; k++) s += ui_2d_ldf(v0 + (uintptr_t)(base + k) * 0x18u + 4);
    return cnt > 0 ? s / (float)cnt : 0.0f;
}

/* y box of the vertices of group g - the quad's drawn extent, which is what
 * has to touch for two quads to count as one tiling element */
static void ui_2d_group_ry(uint8_t* v0, int n, int g, float* lo, float* hi) {
    int base = g * g_ui_2d_vpq, cnt = n - base, k;
    float a = 9.0e9f, b = -9.0e9f;

    if (cnt > g_ui_2d_vpq) cnt = g_ui_2d_vpq;
    for (k = 0; k < cnt; k++) {
        float y = ui_2d_ldf(v0 + (uintptr_t)(base + k) * 0x18u + 4);
        if (y < a) a = y;
        if (y > b) b = y;
    }
    if (cnt > 0) { *lo = a; *hi = b; } else { *lo = 0.0f; *hi = 0.0f; }
}

/* x box of the vertices of group g - how wide the quad is drawn */
static void ui_2d_group_rx(uint8_t* v0, int n, int g, float* lo, float* hi) {
    int base = g * g_ui_2d_vpq, cnt = n - base, k;
    float a = 9.0e9f, b = -9.0e9f;

    if (cnt > g_ui_2d_vpq) cnt = g_ui_2d_vpq;
    for (k = 0; k < cnt; k++) {
        float x = ui_2d_ldf(v0 + (uintptr_t)(base + k) * 0x18u);
        if (x < a) a = x;
        if (x > b) b = x;
    }
    if (cnt > 0) { *lo = a; *hi = b; } else { *lo = 0.0f; *hi = 0.0f; }
}

/* quads whose centres sit within ui_2d_cluster of each other are one element:
 * a text run, a row of hud icons, one nine-slice panel. but a centre rule
 * alone chains whole text blocks together - pause rows sit 0.14 apart - and
 * scaling such a block about its own mean slides the outer lines towards the
 * middle. so a glyph-sized quad that sits further than ui_2d_rowgap from the
 * run so far starts its own element even when the centres are close: each
 * line keeps the y the artist authored. the guards keep the cut to text -
 * a glyph is small and narrow, the run it leaves is wide, and it takes
 * UI_2D_ROW_N quads to make a line - so tiling pieces (nine-slice strips,
 * roof tiles, a sprite column) and the camp rope (0.03 wide) never split.
 * returns the first quad past the run and leaves the run's mean y centre in
 * *mean. */
static int ui_2d_cluster(uint8_t* v0, int n, int g0, float* mean) {
    int ng = (n + g_ui_2d_vpq - 1) / g_ui_2d_vpq;
    int g = g0 + 1;
    float cprev = ui_2d_group_cy(v0, n, g0);
    float sum = cprev;
    float ylo, yhi, xlo, xhi;
    float hprev, q0p, q1p;

    ui_2d_group_ry(v0, n, g0, &ylo, &yhi);
    ui_2d_group_rx(v0, n, g0, &xlo, &xhi);
    ui_2d_group_ry(v0, n, g0, &q0p, &q1p);
    hprev = q1p - q0p;

    while (g < ng) {
        float c = ui_2d_group_cy(v0, n, g);
        float q0, q1, w0, w1;
        int strip;

        ui_2d_group_ry(v0, n, g, &q0, &q1);
        ui_2d_group_rx(v0, n, g, &w0, &w1);
        /* a quad that continues a vertical strip (same x box, touching in y)
         * is merged whatever its centre distance: the centre rule is for
         * separate elements, and a rope drawn in one batch has a pitch above
         * 0.20 in its last frames (shape 272, 0.223). cut there, every
         * segment scaled 0.75 about its own centre and 25% gaps opened. the
         * merged column spans the frame (2.002) and touches the edges, so the
         * fill exemption leaves it unscaled: a continuous rope at its
         * authored extents, which is intended. */
        strip = ui_2d_absf(w0 - xlo) <= UI_2D_STRIP_X
             && ui_2d_absf(w1 - xhi) <= UI_2D_STRIP_X
             && (q0 > yhi ? q0 - yhi : (ylo > q1 ? ylo - q1 : 0.0f)) <= UI_2D_STRIP_GAP;
        if (!strip && ui_2d_absf(c - cprev) > g_ui_2d_cluster) break;
        /* ui_2d_bars: backing bars of equal height whose centres are offset by
         * less than 0.75 h are two stacked levels (pause / confirm rows). the
         * 0.20 centre rule welds them and the shared mean moves each bar about
         * 0.018 off its text row. cutting here is safe: each piece scales 0.75
         * about its own centre and still overlaps its neighbour by >= 0.25 h,
         * so no gap opens. glyph rows (dc ~ 0) and abutting tiles (dc ~ h)
         * never match. */
        if (g_ui_2d_bars) {
            float h1 = q1 - q0, dc = ui_2d_absf(c - cprev);
            if (ui_2d_absf(h1 - hprev) <= 0.01f && dc >= UI_2D_BAR_D
                && dc <= 0.75f * h1) break;
        }
        if (g_ui_2d_rowgap > 0.0f) {
            float gap = q0 > yhi ? q0 - yhi : (ylo > q1 ? ylo - q1 : 0.0f);
            if (gap > g_ui_2d_rowgap && (q1 - q0) <= UI_2D_ROW_H
                && (w1 - w0) <= UI_2D_ROW_W && (xhi - xlo) >= UI_2D_ROW_RUN
                && (g - g0) >= UI_2D_ROW_N) break;
        }
        if (q0 < ylo) ylo = q0;
        if (q1 > yhi) yhi = q1;
        if (w0 < xlo) xlo = w0;
        if (w1 > xhi) xhi = w1;
        sum += c;
        cprev = c;
        hprev = q1 - q0;
        g++;
    }
    *mean = sum / (float)(g - g0);
    return g;
}

/* one cluster alone hugs a screen edge when it is compact, sits past the pin
 * line and lives out in a screen corner - a hud icon, a short text run. the
 * corner test is what keeps the camp rope honest: its top segments are
 * compact runs past 0.85 sitting dead centre, and pinning them made each
 * segment jump a dozen pixels while the segments below stayed put, so the
 * rope visibly broke. the batch-wide decision lives in ui_2d_batch_pins: only
 * a batch where every cluster is such a corner element is treated as hud and
 * pinned. anything else - the choose-adventurer camp, the banner, a menu - is
 * one piece of artwork emitted as several clusters, and pinning a part of it
 * would tear the picture apart, so those scale about their own lines and
 * every position the artist authored is preserved. */
static int ui_2d_cluster_edge(uint8_t* v0, int n, int g0, int g1, float mean) {
    float ymin = 9.0e9f, ymax = -9.0e9f, xmin = 9.0e9f, xmax = -9.0e9f, xc;
    int last = g1 * g_ui_2d_vpq, k;

    if (last > n) last = n;
    for (k = g0 * g_ui_2d_vpq; k < last; k++) {
        float x = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u);
        float y = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u + 4);
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    if ((ymax - ymin) > g_ui_2d_pinspan) return 0;
    xc = (xmin + xmax) * 0.5f;
    if (xc < 0.0f) xc = -xc;
    if (xc < g_ui_2d_pinx) return 0;
    return mean >= g_ui_2d_pin || mean <= -g_ui_2d_pin;
}

static int ui_2d_batch_pins(uint8_t* v0, int n) {
    int ng = (n + g_ui_2d_vpq - 1) / g_ui_2d_vpq, g = 0;

    if (ng <= 0) return 0;
    while (g < ng) {
        float mean;
        int g1 = ui_2d_cluster(v0, n, g, &mean);
        if (!ui_2d_cluster_edge(v0, n, g, g1, mean)) return 0;
        g = g1;
    }
    return 1;
}

static int ui_2d_pinned_runs(uint8_t* v0, int n) {
    int ng = (n + g_ui_2d_vpq - 1) / g_ui_2d_vpq, g = 0, cnt = 0;

    if (!ui_2d_batch_pins(v0, n)) return 0;
    while (g < ng) {
        float mean;
        g = ui_2d_cluster(v0, n, g, &mean);
        cnt++;
    }
    return cnt;
}

/* the chain slot this column segment belongs to: the same x box as a segment
 * drawn earlier, still above this one. -1 when nothing above matches, so a
 * column's first (or lowest) segment starts the column. read-only - the dump
 * calls it too, and it has to see exactly what ui_2d_apply_runs will see. */
static int ui_2d_chain_find(float x0, float x1, float mean) {
    int i;

    for (i = 0; i < UI_2D_CHAIN_N; i++) {
        if (!g_ui_2d_chused[i]) continue;
        if (ui_2d_absf(x0 - g_ui_2d_chx0[i]) > UI_2D_CHAIN_X) continue;
        if (ui_2d_absf(x1 - g_ui_2d_chx1[i]) > UI_2D_CHAIN_X) continue;
        if (g_ui_2d_chcy[i] < mean - 0.005f) continue;
        if (g_ui_2d_chcy[i] > mean + UI_2D_CHAIN_Y) continue;
        return i;
    }
    return -1;
}

/* the anchor a cluster scales about, decided in one place so the dump and the
 * frame trace print exactly what ui_2d_apply_runs uses. *narrow is set for a
 * rope / sprite-column segment and *slot is its chain slot (-1: none yet).
 * read-only. ui_2d_center sends a cluster on the screen's centre line to
 * y = 0: a centred overlay is a board with its text, and the letterbox
 * scales all of it about 0, while own-centre anchors left the text where it
 * was authored and hung it off the shrunk board. */
static float ui_2d_anchor(int pins, float mean, float xmin, float xmax, float ymin, float ymax,
                          int* slot, int* narrow) {
    float a = pins ? (mean >= 0.0f ? 1.0f : -1.0f) : mean;

    *slot = -1;
    *narrow = 0;
    if ((xmax - xmin) <= UI_2D_ROW_W && (ymax - ymin) <= UI_2D_ROW_H) {
        *narrow = 1;
        *slot = ui_2d_chain_find(xmin, xmax, mean);
        /* a column keeps the anchor of its top segment even when the batch
         * pins: a rope tip that crosses the pin line must not snap to the
         * bottom edge (it tore the rope ~270 px) */
        if (*slot >= 0) a = g_ui_2d_chanchor[*slot];
    }
    if (g_ui_2d_center && !pins && xmin >= -1.0f && xmax <= 1.0f
        && ui_2d_absf((xmin + xmax) * 0.5f) <= UI_2D_CENTER_X) a = 0.0f;
    return a;
}

/* a cluster that is left at full height: a fade / dim / blur fill or a
 * backdrop that covers the frame (scaling one opens a band at its edges, or
 * pulls it off the fill it was drawn to line up with), a full-frame image
 * mid-zoom, or the camp's centre rope that zooms with it. ngroups is the
 * cluster's quad count. */
static int ui_2d_keep(int ngroups, float xmin, float xmax, float ymin, float ymax) {
    float h = ymax - ymin, w = xmax - xmin;

    if (h >= UI_2D_FILL_SPAN) return 1;
    if (h >= UI_2D_FRAME_MIN && ui_2d_absf(w - h) <= UI_2D_FRAME_TOL * h) return 1;
    if (ngroups >= UI_2D_ROPE_N && w <= UI_2D_ROPE_W && h >= UI_2D_FRAME_MIN) return 1;
    return 0;
}

static int ui_2d_apply_runs(uint8_t* v0, int n, float sy, float sx) {
    int ng = (n + g_ui_2d_vpq - 1) / g_ui_2d_vpq, g = 0, scaled = 0;
    int pins = ui_2d_batch_pins(v0, n);
    int fill = g_ui_2d_fill;

    while (g < ng) {
        float mean, anchor, ymin = 9.0e9f, ymax = -9.0e9f, xmin = 9.0e9f, xmax = -9.0e9f;
        int g1 = ui_2d_cluster(v0, n, g, &mean);
        int k, last;

        last = g1 * g_ui_2d_vpq;
        if (last > n) last = n;
        for (k = g * g_ui_2d_vpq; k < last; k++) {
            float x = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u);
            float y = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u + 4);
            if (x < xmin) xmin = x;
            if (x > xmax) xmax = x;
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }

        /* pinned: keep the distance to the edge the element hugs; otherwise
         * scale about the element's own centre, which does not move it. a
         * narrow glyph-sized cluster is a rope / sprite-column segment: the
         * column is drawn one segment to a batch, so the segment above it is
         * long gone by the time this one arrives and the only way to hold the
         * column on one anchor is to remember it. take the column's anchor,
         * else hand this quad's own centre to the segments below. the test is
         * per cluster, not per batch: the camp flushes both of its banner
         * ropes in one batch (n 8, two narrow clusters). */
        {
            int s, narrow;
            anchor = ui_2d_anchor(pins, mean, xmin, xmax, ymin, ymax, &s, &narrow);
            if (narrow) {
                if (s < 0) {
                    s = g_ui_2d_chnext;
                    g_ui_2d_chnext = (g_ui_2d_chnext + 1) % UI_2D_CHAIN_N;
                }
                g_ui_2d_chused[s] = 1;
                g_ui_2d_chx0[s] = xmin;
                g_ui_2d_chx1[s] = xmax;
                g_ui_2d_chcy[s] = mean;
                g_ui_2d_chanchor[s] = anchor;
            }
        }

        /* fills, backdrops, full-frame images and the scene rope are left at
         * full height, see ui_2d_keep */
        if (fill && ui_2d_keep(g1 - g, xmin, xmax, ymin, ymax)) {
            g = g1;
            continue;
        }
        for (k = g * g_ui_2d_vpq; k < last; k++) {
            uint8_t* v = v0 + (uintptr_t)k * 0x18u;
            float y = ui_2d_ldf(v + 4);
            /* world geometry rides the same flush with y in the tens */
            if (y < -UI_2D_NDC_Y || y > UI_2D_NDC_Y) continue;
            /* ui_2d_fill = 0: the old per-vertex line, so a vertex past
             * ui_2d_edge stays where it is and the rest compresses under it */
            if (!fill && (y < -g_ui_2d_edge || y > g_ui_2d_edge)) continue;
            ui_2d_stf(v + 4, anchor + (y - anchor) * sy);
            if (sx != 1.0f) ui_2d_stf(v, ui_2d_ldf(v) * sx);
            scaled++;
        }
        g = g1;
    }
    return scaled;
}

/* one line per distinct batch shape, so a single device run says where every
 * pass sits - vertex counts, x/y/z range, quad centres - with no per-frame spam */
static uint32_t g_ui_2d_shapes[1200];
static int      g_ui_2d_nshapes = 0;
static int      g_ui_2d_boxes   = 0;
static int      g_ui_2d_lvboxes = 0;

static void ui_2d_append_f(char* b, size_t cap, float v) {
    size_t p = my_strlen(b);
    if (p + 1 < cap) b[p++] = ' ';
    if (p < cap) fmt_float(v, 3, b + p, cap - p);
}

static void ui_2d_append_s(char* b, size_t cap, const char* s) {
    size_t p = my_strlen(b);
    while (*s && p + 1 < cap) b[p++] = *s++;
    if (p < cap) b[p] = '\0';
}

static void ui_2d_dump_shape(uint8_t* v0, int n, int nidx, int stride, int pinned) {
    float xmin = 9.0e9f, xmax = -9.0e9f, ymin = 9.0e9f, ymax = -9.0e9f, zmin = 9.0e9f, zmax = -9.0e9f;
    uint32_t sig;
    char b[384], t1[16], t2[16], t3[16], t4[16], t5[16];
    int i, k, ng;

    if (!g_do_ui_2d_dump) return;

    for (k = 0; k < n; k++) {
        const uint8_t* v = v0 + (uintptr_t)k * 0x18u;
        float x = ui_2d_ldf(v), y = ui_2d_ldf(v + 4), z = ui_2d_ldf(v + 8);
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
        if (z < zmin) zmin = z;
        if (z > zmax) zmax = z;
    }

    /* coarse buckets, so an element sliding into place does not log per pixel */
    sig = (uint32_t)n * 2654435761u + (uint32_t)nidx * 40503u
        + (uint32_t)((ymin + 8.0f) * 8.0f) * 31u
        + (uint32_t)((ymax + 8.0f) * 8.0f) * 9973u;
    for (i = 0; i < g_ui_2d_nshapes; i++) if (g_ui_2d_shapes[i] == sig) return;
    if (g_ui_2d_nshapes >= 1200) return;

    /* only the ndc passes: world geometry rides through the same flush with y in
     * the tens and would eat the whole table */
    if (ymin < -8.0f || ymax > 8.0f) return;
    g_ui_2d_shapes[g_ui_2d_nshapes++] = sig;

    fmt_int(g_ui_2d_nshapes, t1, sizeof(t1));
    fmt_int(n, t2, sizeof(t2));
    fmt_int(nidx, t3, sizeof(t3));
    fmt_int(stride, t4, sizeof(t4));
    fmt_int(pinned, t5, sizeof(t5));
    my_format(b, sizeof(b), "[2d] shape %s b %d n %s idx %s stride %s pin %s v %d",
              t1, g_ui_2d_batches, t2, t3, t4, t5, g_ui_2d_vpq);
    log_raw(b);

    my_format(b, sizeof(b), "[2d]   x");
    ui_2d_append_f(b, sizeof(b), xmin);
    ui_2d_append_f(b, sizeof(b), xmax);
    ui_2d_append_s(b, sizeof(b), "  y");
    ui_2d_append_f(b, sizeof(b), ymin);
    ui_2d_append_f(b, sizeof(b), ymax);
    ui_2d_append_s(b, sizeof(b), "  z");
    ui_2d_append_f(b, sizeof(b), zmin);
    ui_2d_append_f(b, sizeof(b), zmax);
    log_raw(b);

    ng = n / g_ui_2d_vpq;
    if (ng > 12) ng = 12;
    if (ng > 0) {
        my_format(b, sizeof(b), "[2d]   c");
        for (k = 0; k < ng; k++) ui_2d_append_f(b, sizeof(b), ui_2d_group_cy(v0, n, k));
        log_raw(b);
    }

    /* what this batch was decided to be: mean y, span, anchor per cluster.
     * the anchor is the one ui_2d_apply_runs is about to use, chain lookup
     * included - printing the pin-or-own-centre answer instead made a working
     * rope chain and a broken one look identical in the log. */
    {
        int g = 0, shown = 0;
        int pins = ui_2d_batch_pins(v0, n);
        char bb[160];
        ng = (n + g_ui_2d_vpq - 1) / g_ui_2d_vpq;
        my_format(b, sizeof(b), "[2d]   g");
        my_format(bb, sizeof(bb), "[2d]   bb");
        while (g < ng && shown < 8) {
            float mean, tmin = 9.0e9f, tmax = -9.0e9f, tx0 = 9.0e9f, tx1 = -9.0e9f, a;
            int g1 = ui_2d_cluster(v0, n, g, &mean), last = g1 * g_ui_2d_vpq, j;

            if (last > n) last = n;
            for (j = g * g_ui_2d_vpq; j < last; j++) {
                float x = ui_2d_ldf(v0 + (uintptr_t)j * 0x18u);
                float y = ui_2d_ldf(v0 + (uintptr_t)j * 0x18u + 4);
                if (x < tx0) tx0 = x;
                if (x > tx1) tx1 = x;
                if (y < tmin) tmin = y;
                if (y > tmax) tmax = y;
            }
            {
                int s, nr;
                a = ui_2d_anchor(pins, mean, tx0, tx1, tmin, tmax, &s, &nr);
            }
            ui_2d_append_f(b, sizeof(b), mean);
            ui_2d_append_f(b, sizeof(b), tmax - tmin);
            ui_2d_append_f(b, sizeof(b), a);
            ui_2d_append_f(bb, sizeof(bb), (tmin + tmax) * 0.5f);
            g = g1;
            shown++;
        }
        log_raw(b);
        log_raw(bb);
    }

    /* rope-like columns and big backdrops get their quads listed: cy, height
     * and width each, 4 quads a line, 12 shapes max. a tiling strip that the
     * row split cut apart - or a dim that stopped covering the frame - shows
     * up here as gaps between neighbouring boxes. a is the first vertex's
     * alpha, which tells a dim (a < 255) from drawn art. */
    {
        int old = g_ui_2d_boxes < 12
            && (((xmax - xmin) <= 0.15f && (ymax - ymin) >= 0.25f)
                || (ymax - ymin) >= 1.3f);
        int lv = 0, gc = (n + g_ui_2d_vpq - 1) / g_ui_2d_vpq;

        /* multi-level batch (pause / confirm backing, camp art): consecutive
         * groups whose centres jump 0.05-0.20. own cap, so it cannot starve
         * the rope / backdrop boxes. decides whether ui_2d_bars is safe. */
        if (!old && g_ui_2d_lvboxes < 24) {
            for (k = 1; k < gc; k++) {
                float d = ui_2d_absf(ui_2d_group_cy(v0, n, k) - ui_2d_group_cy(v0, n, k - 1));
                if (d >= 0.05f && d <= 0.20f) { lv = 1; break; }
            }
        }
        if (old || lv) {
        char ta[16];
        int gcount = gc;
        if (old) g_ui_2d_boxes++; else g_ui_2d_lvboxes++;
        fmt_int((int)((*(uint32_t*)(v0 + 0x14) >> 24) & 0xffu), ta, sizeof(ta));
        my_format(b, sizeof(b), "[2d]   a %s", ta);
        log_raw(b);
        if (gcount > 24) gcount = 24;
        my_format(b, sizeof(b), "[2d]   q");
        for (k = 0; k < gcount; k++) {
            float q0, q1, w0, w1;
            ui_2d_group_ry(v0, n, k, &q0, &q1);
            ui_2d_group_rx(v0, n, k, &w0, &w1);
            ui_2d_append_f(b, sizeof(b), (q0 + q1) * 0.5f);
            ui_2d_append_f(b, sizeof(b), q1 - q0);
            ui_2d_append_f(b, sizeof(b), w1 - w0);
            if ((k % 4) == 3 || k == gcount - 1) {
                log_raw(b);
                my_format(b, sizeof(b), "[2d]   q");
            }
        }
        }
    }
}

/* frame trace: a ring of the last 96 flushed ndc batches. the shape dump
 * dedups, so it cannot say which batches make up a pause / confirm frame (the
 * board, the banner, the stone face never show up next to the text). a
 * triangle-list text batch that is wide and centred (pause text n 312,
 * confirm n 480) dumps the ring, then the next 24 batches as they arrive. */
#define UI_2D_TR_N     96
#define UI_2D_TR_AFTER 24
#define UI_2D_TR_MAX   4
static struct { int b, n, idx, vpq, al; float x0, x1, y0, y1, anc; } g_ui_2d_tr[UI_2D_TR_N];
static int g_ui_2d_trhead = 0, g_ui_2d_trcount = 0;
static int g_ui_2d_trfired = 0, g_ui_2d_trafter = 0, g_ui_2d_trlastn = -1, g_ui_2d_trseq = 0;

static void ui_2d_tr_log(int i) {
    char b[256], t1[16], t2[16], t3[16], t4[16], t5[16], t6[16];

    fmt_int(g_ui_2d_trseq++, t1, sizeof(t1));
    fmt_int(g_ui_2d_tr[i].b, t2, sizeof(t2));
    fmt_int(g_ui_2d_tr[i].n, t3, sizeof(t3));
    fmt_int(g_ui_2d_tr[i].idx, t4, sizeof(t4));
    fmt_int(g_ui_2d_tr[i].vpq, t5, sizeof(t5));
    fmt_int(g_ui_2d_tr[i].al, t6, sizeof(t6));
    my_format(b, sizeof(b), "[2d] tr %s b %s n %s idx %s v %s a %s", t1, t2, t3, t4, t5, t6);
    ui_2d_append_s(b, sizeof(b), "  x");
    ui_2d_append_f(b, sizeof(b), g_ui_2d_tr[i].x0);
    ui_2d_append_f(b, sizeof(b), g_ui_2d_tr[i].x1);
    ui_2d_append_s(b, sizeof(b), "  y");
    ui_2d_append_f(b, sizeof(b), g_ui_2d_tr[i].y0);
    ui_2d_append_f(b, sizeof(b), g_ui_2d_tr[i].y1);
    ui_2d_append_s(b, sizeof(b), "  anc");
    ui_2d_append_f(b, sizeof(b), g_ui_2d_tr[i].anc);
    log_raw(b);
}

/* called before ui_2d_apply_runs, so the anchor is the one about to be used */
static void ui_2d_trace(uint8_t* v0, int n, int nidx) {
    float x0 = 9.0e9f, x1 = -9.0e9f, y0 = 9.0e9f, y1 = -9.0e9f, mean;
    int k, g1, s, nr, e;

    if (!g_do_ui_2d_dump) return;
    for (k = 0; k < n; k++) {
        float x = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u);
        float y = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u + 4);
        if (y < -UI_2D_NDC_Y || y > UI_2D_NDC_Y) return;   /* world, not ndc */
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    e = g_ui_2d_trhead;
    g_ui_2d_tr[e].b = g_ui_2d_batches;
    g_ui_2d_tr[e].n = n;
    g_ui_2d_tr[e].idx = nidx;
    g_ui_2d_tr[e].vpq = g_ui_2d_vpq;
    g_ui_2d_tr[e].al = (int)((*(uint32_t*)(v0 + 0x14) >> 24) & 0xffu);
    g_ui_2d_tr[e].x0 = x0;
    g_ui_2d_tr[e].x1 = x1;
    g_ui_2d_tr[e].y0 = y0;
    g_ui_2d_tr[e].y1 = y1;
    {
        float cx0 = 9.0e9f, cx1 = -9.0e9f, cy0 = 9.0e9f, cy1 = -9.0e9f;
        int last;
        g1 = ui_2d_cluster(v0, n, 0, &mean);
        last = g1 * g_ui_2d_vpq;
        if (last > n) last = n;
        for (k = 0; k < last; k++) {
            float x = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u);
            float y = ui_2d_ldf(v0 + (uintptr_t)k * 0x18u + 4);
            if (x < cx0) cx0 = x;
            if (x > cx1) cx1 = x;
            if (y < cy0) cy0 = y;
            if (y > cy1) cy1 = y;
        }
        g_ui_2d_tr[e].anc = ui_2d_anchor(ui_2d_batch_pins(v0, n), mean, cx0, cx1, cy0, cy1, &s, &nr);
    }
    g_ui_2d_trhead = (e + 1) % UI_2D_TR_N;
    if (g_ui_2d_trcount < UI_2D_TR_N) g_ui_2d_trcount++;

    if (g_ui_2d_trafter > 0) {
        ui_2d_tr_log(e);
        if (--g_ui_2d_trafter == 0) log_raw("[2d] tr end");
        return;
    }
    if (g_ui_2d_trfired < UI_2D_TR_MAX && g_ui_2d_vpq == 6 && n >= 300
        && ui_2d_absf(x0 + x1) <= 0.04f && n != g_ui_2d_trlastn) {
        int i, c = g_ui_2d_trcount, h = g_ui_2d_trhead;
        char b[96], t1[16], t2[16];
        g_ui_2d_trfired++;
        g_ui_2d_trlastn = n;
        g_ui_2d_trafter = UI_2D_TR_AFTER;
        fmt_int(g_ui_2d_trfired, t1, sizeof(t1));
        fmt_int(n, t2, sizeof(t2));
        my_format(b, sizeof(b), "[2d] tr begin %s trigger n %s ring %d", t1, t2, c);
        log_raw(b);
        for (i = 0; i < c; i++) ui_2d_tr_log((h - c + i + 2 * UI_2D_TR_N) % UI_2D_TR_N);
    }
}

static void ui_2d_scale_batch(void* dispp) {
    static int logged = 0;
    uint8_t* disp = (uint8_t*)dispp;
    int cw = 0, ch = 0, stride = 0, n = 0, nidx = 0, scaled = 0, pinned = 0;
    float sy = 0.0f;
    uint8_t* end = 0;

    if (!disp) return;
    cw = *(int*)(disp + 8);
    ch = *(int*)(disp + 0xc);
    if (cw > 0 && ch > 0) sy = ((float)cw * 9.0f / 16.0f) / (float)ch;
    stride = ui_2d_stride(disp);
    n    = *(int*)(disp + 0x8031c);
    nidx = *(int*)(disp + 0x80318);
    end  = *(uint8_t**)(disp + 0x802f8);

    if (!logged && (n > 0 || nidx > 0)) {
        char b[240], scw[16], sch[16], sst[16], sn[16], si[16], ssy[32];
        logged = 1;
        fmt_int(cw, scw, sizeof(scw));
        fmt_int(ch, sch, sizeof(sch));
        fmt_int(stride, sst, sizeof(sst));
        fmt_int(n, sn, sizeof(sn));
        fmt_int(nidx, si, sizeof(si));
        fmt_float(sy, 4, ssy, sizeof(ssy));
        my_format(b, sizeof(b), "[2d] first flush: canvas %s x %s stride %s verts %s idx %s sy %s",
                  scw, sch, sst, sn, si, ssy);
        log_raw(b);
    }

    /* vertices per element: 4 for indexed quads, 6 when the quad ships as its
     * two triangles (idx == n). clamped so a stray index count cannot make a
     * group swallow several elements. */
    g_ui_2d_vpq = 4;
    if (nidx > 0 && (nidx % 6) == 0) {
        int v = n * 6 / nidx;
        if (v >= 3 && v <= 6) g_ui_2d_vpq = v;
    }

    /* the canvas changes as the renderer enters a screen and the 2d layer
     * snaps with it: log every change so the transition reads as a timeline
     * (the camp's first second runs at 1280x720, sy 1.0, nothing scaled) */
    {
        static int lcw = 0, lch = 0;
        if (cw != lcw || ch != lch) {
            char b2[200], s1[16], s2[16], s3[32], s4[16];
            lcw = cw;
            lch = ch;
            fmt_int(cw, s1, sizeof(s1));
            fmt_int(ch, s2, sizeof(s2));
            fmt_float(sy, 4, s3, sizeof(s3));
            fmt_int(g_ui_2d_batches, s4, sizeof(s4));
            my_format(b2, sizeof(b2), "[2d] canvas %s x %s sy %s batches %s",
                      s1, s2, s3, s4);
            log_raw(b2);
        }
    }

    /* 1.0 when the canvas is already 16:9, 0.75 at 1280x960 */
    if (!(sy > 0.05f && sy < 0.999f)) return;
    if (stride != 0x18) return;
    if (!end || n <= 0 || n > 0x80000) return;

    {
        uint8_t* v = end - (uintptr_t)n * 0x18u;
        pinned = ui_2d_pinned_runs(v, n);
        ui_2d_dump_shape(v, n, nidx, stride, pinned);
        ui_2d_trace(v, n, nidx);
        scaled = ui_2d_apply_runs(v, n, sy, g_ui_2d_scale_x);
    }

    g_ui_2d_batches++;
    g_ui_2d_verts += scaled;
    if (g_ui_2d_batches <= 3 || (g_ui_2d_batches % 2000) == 0) {
        char b[200], sn[16], sv[16], sb[16], sp[16];
        fmt_int(n, sn, sizeof(sn));
        fmt_int(scaled, sv, sizeof(sv));
        fmt_int(g_ui_2d_batches, sb, sizeof(sb));
        fmt_int(pinned, sp, sizeof(sp));
        my_format(b, sizeof(b), "[2d] batch %s: %s of %s verts scaled, %s edge-pinned", sb, sv, sn, sp);
        log_raw(b);
    }
}

static void ui_2d_flush_hook(void* disp) {
    if (g_do_ui_2d) ui_2d_scale_batch(disp);
    ui_flush_body_enter(disp);
}

static void patch_ui_2d(uintptr_t base) {
    /* 0x22cc5d80: 41 57 41 56 56 57 53 48 83 ec 60 48 89 ce
     *             push r15 / r14 / rsi / rdi / rbx ; sub rsp,0x60 ; mov rsi,rcx
     * 12 of those 14 bytes are replaced with
     *   mov rax, <ui_2d_flush_hook> ; jmp rax
     * the hook scales the pending batch, replays the 14 bytes and jumps to
     * the body at 0x22cc5d8e, so the original epilogue still unwinds the
     * frame and returns to whoever called the flush. */
    static const unsigned char expect[12] = {
        0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53,
        0x48, 0x83, 0xec, 0x60, 0x48
    };
    unsigned char repl[12];
    uint64_t fn = (uint64_t)(uintptr_t)&ui_2d_flush_hook;

    ui_flush_body_va = base + RVA_UI_FLUSH_BODY;

    my_memcpy(repl, expect, sizeof(repl));
    repl[0] = 0x48;
    repl[1] = 0xb8;
    my_memcpy(repl + 2, &fn, 8);
    repl[10] = 0xff;
    repl[11] = 0xe0;

    patch_bytes(base + RVA_UI_FLUSH, expect, repl, sizeof(repl), "ui_2d_scale");
}

static void ui_draw_letterboxed(void) {
    void* d = (void*)*(uintptr_t*)(g_base + RVA_UI_DISP_CACHE);
    if (!d) return;

    int cw = *(int*)((uint8_t*)d + 8);
    int ch = *(int*)((uint8_t*)d + 0xc);
    int vw = 0, vh = 0;
    void** vt = *(void***)d;

    typedef void (*set_viewport_fn)(void*, int, int, int, int);
    set_viewport_fn set_vp = vt ? (set_viewport_fn)vt[0x48 / 8] : 0;

    if (set_vp && cw > 0 && ch > 0) {
        /* widest 16:9 rect that fits the canvas, centred */
        vw = cw; vh = (cw * 9) / 16;
        if (vh > ch) { vh = ch; vw = (ch * 16) / 9; }
        set_vp(d, (cw - vw) / 2, (ch - vh) / 2, vw, vh);
    }

    /* the draw we were spliced in front of */
    ui_flush_body_enter(d);

    /* put the canvas viewport back. 95818d0 left the band installed and every
     * later pass inherited it - that is what letterboxed the menus. */
    if (set_vp && vw > 0) set_vp(d, 0, 0, cw, ch);

    /* the two stores we displaced */
    *(uint8_t*)((uint8_t*)d + 0x802ad) = 0x37;
    *(uint32_t*)((uint8_t*)d + 0x80318) = 0;
}

static void patch_ui_letterbox(uintptr_t base) {
    /* 0x22c5e12d: e8 4e 7c 06 00                  call 0x22cc5d80  <- the draw
     * 0x22c5e132: c6 86 ad 02 08 00 37             mov byte  [rsi+0x802ad], 0x37
     * 0x22c5e139: c7 86 18 03 08 00 00 00 00 00    mov dword [rsi+0x80318], 0
     *
     * 22 bytes, replaced with mov rax,<stub> ; call rax ; 10 nops. the stub
     * sets the band, makes the real draw happen inside it, restores the
     * canvas viewport and only then does the two stores - so the hud sits in
     * the band and nothing drawn later inherits it. */
    static const unsigned char expect[22] = {
        0xe8, 0x4e, 0x7c, 0x06, 0x00,
        0xc6, 0x86, 0xad, 0x02, 0x08, 0x00, 0x37,
        0xc7, 0x86, 0x18, 0x03, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    unsigned char repl[22];
    uint64_t fn = (uint64_t)(uintptr_t)&ui_draw_letterboxed;
    int i;

    ui_flush_body_va = base + RVA_UI_FLUSH_BODY;

    my_memcpy(repl, expect, sizeof(repl));
    repl[0] = 0x48;
    repl[1] = 0xb8;
    my_memcpy(repl + 2, &fn, 8);
    repl[10] = 0xff;
    repl[11] = 0xd0;
    for (i = 12; i < 22; i++) repl[i] = 0x90;

    patch_bytes(base + RVA_UI_DRAW_HOOK, expect, repl, sizeof(repl), "ui_letterbox");
}

/* ### ui quad aspect correction

 * ui quads in .rdata are baked with w/h = 9/16 = 0.5625 so they render square
 * through the stock 16:9 projection. with the projection matrix now at 4:3
 * (m00/m11 = 0.75) they come out at:
 *
 *   pixel_w / pixel_h = 0.5625 * 0.75 * (1280/960) = 0.5625
 *
 * so x needs * 1/0.5625 = 16/9 to land back on square pixels. note this is
 * ORIGINAL_ASPECT, NOT ORIGINAL_ASPECT/target_aspect -- dividing by the target
 * aspect is what broke the menus before. */

static const uint32_t UI_QUADS_8F[] = {
    0x22d0df30u, 0x22d0e0f0u, 0x22d0e110u, 0x22d0e1d0u, 0x22d0e2a0u, 0x22d0e2c0u,
    0x22d0fe50u, 0x22d0fe90u, 0x22d0fed0u, 0x22d0ffa0u, 0x22d0ffc0u, 0x22d10270u,
    0x22d10290u, 0x22d102d0u, 0x22d103f0u, 0x22d10410u, 0x22d111c0u, 0x22d11270u,
    0x22d11290u, 0x22d112b0u, 0x22d11370u, 0x22d113b0u, 0x22d114d0u, 0x22d11640u,
    0x22d116b0u, 0x22d11710u, 0x22d119c0u, 0x22d11a80u, 0x22d11aa0u, 0x22d11b40u,
    0x22d11ed0u, 0x22d11ef0u, 0x22d11f10u
};

static const uint32_t UI_QUADS_6F[] = {
    0x22d0dd38u, 0x22d0dd58u, 0x22d10bf8u
};

static const uint32_t UI_PAIRS[] = {
    0x22d0eb70u, 0x22d0eb78u, 0x22d0eb90u, 0x22d0eb98u,
    0x22d0ec18u, 0x22d0ec28u, 0x22d0ec30u, 0x22d0ec38u,
    0x22d0ec50u, 0x22d0ec58u, 0x22d0f030u, 0x22d0f038u,
    0x22d0f050u, 0x22d0f058u, 0x22d0f4b8u, 0x22d0f680u,
    0x22d0f698u, 0x22d10974u, 0x22d11990u
};

static void patch_ui_quads(uintptr_t base, float scale_x, float scale_y) {
    /* .rdata quads are baked w/h = 9/16 for the stock 16:9 projection. through
     * the 4:3 matrix (m00/m11 = 0.75) they land too narrow, but the
     * theoretical 1.7778 overcorrects. both axes are dialled from the ini.
     *
     * NOTE: these values are written into .rdata, which persists for the life
     * of the process. the w/h == 9/16 filter only matches pristine quads, so a
     * second apply_patches in the same process is a no-op. that is intended. */
    int patched_8f = 0, patched_6f = 0, patched_pairs = 0;
    DWORD old = 0;

    int n8 = (int)(sizeof(UI_QUADS_8F) / sizeof(UI_QUADS_8F[0]));
    for (int i = 0; i < n8; i++) {
        uintptr_t addr = base + UI_QUADS_8F[i];
        float v[8];
        if (!VirtualProtect((LPVOID)addr, sizeof(v), PAGE_EXECUTE_READWRITE, &old)) continue;
        my_memcpy(v, (void*)addr, sizeof(v));
        float w = fabsf(v[2]);
        float h = fabsf(v[1]);
        if (h > 0.0001f && fabsf((w / h) - ORIGINAL_INV) < 0.005f) {
            float nw = w * scale_x;
            float nh = h * scale_y;
            v[0] = -nw; v[2] = nw; v[4] = -nw; v[6] = nw;
            v[1] = -nh; v[3] = -nh; v[5] = nh; v[7] = nh;
            my_memcpy((void*)addr, v, sizeof(v));
            patched_8f++;
        }
        VirtualProtect((LPVOID)addr, sizeof(v), old, &old);
    }

    int n6 = (int)(sizeof(UI_QUADS_6F) / sizeof(UI_QUADS_6F[0]));
    for (int i = 0; i < n6; i++) {
        uintptr_t addr = base + UI_QUADS_6F[i];
        float v[6];
        if (!VirtualProtect((LPVOID)addr, sizeof(v), PAGE_EXECUTE_READWRITE, &old)) continue;
        my_memcpy(v, (void*)addr, sizeof(v));
        float w = fabsf(v[2]);
        float h = fabsf(v[1]);
        if (h > 0.0001f && fabsf((w / h) - ORIGINAL_INV) < 0.005f) {
            v[0] = v[0] * scale_x;
            v[2] = v[2] * scale_x;
            v[1] = v[1] * scale_y;
            v[3] = v[3] * scale_y;
            v[4] = v[4] * scale_y;
            v[5] = v[5] * scale_y;
            my_memcpy((void*)addr, v, sizeof(v));
            patched_6f++;
        }
        VirtualProtect((LPVOID)addr, sizeof(v), old, &old);
    }

    int np = (int)(sizeof(UI_PAIRS) / sizeof(UI_PAIRS[0]));
    for (int i = 0; i < np; i++) {
        uintptr_t addr = base + UI_PAIRS[i];
        float v[2];
        if (!VirtualProtect((LPVOID)addr, sizeof(v), PAGE_EXECUTE_READWRITE, &old)) continue;
        my_memcpy(v, (void*)addr, sizeof(v));
        float w = fabsf(v[0]);
        float h = fabsf(v[1]);
        if (h > 0.0001f && fabsf((w / h) - ORIGINAL_INV) < 0.005f) {
            v[0] = v[0] * scale_x;
            my_memcpy((void*)addr, v, sizeof(v));
            patched_pairs++;
        }
        VirtualProtect((LPVOID)addr, sizeof(v), old, &old);
    }

    char buf[160];
    my_format(buf, sizeof(buf), "[*] ui_quads: %d/%d (8f), %d/%d (6f), %d/%d pairs scaled x%d/100 y%d/100",
              patched_8f, n8, patched_6f, n6, patched_pairs, np,
              (int)(scale_x * 100.0f), (int)(scale_y * 100.0f));
    log_raw(buf);

    /* readback proof: the filter only matches pristine 9/16 quads, so if these
     * came back non-9/16 the write demonstrably landed. if they read 9/16 the
     * write was reverted or the addresses are not the ones being rendered. */
    if (n8 > 0) {
        float q[8];
        my_memcpy(q, (void*)(base + UI_QUADS_8F[0]), sizeof(q));
        char sw[24], sh[24], sr[24];
        fmt_float(fabsf(q[2]), 5, sw, sizeof(sw));
        fmt_float(fabsf(q[1]), 5, sh, sizeof(sh));
        float ratio = fabsf(q[1]) > 0.0001f ? fabsf(q[2]) / fabsf(q[1]) : 0.0f;
        fmt_float(ratio, 5, sr, sizeof(sr));
        char probe[160];
        my_format(probe, sizeof(probe), "[*] ui_quads probe 0x%x: w=%s h=%s w/h=%s (0x22d0df30)",
                  UI_QUADS_8F[0], sw, sh, sr);
        log_raw(probe);
    }
}

/* ### surgical HUD horizontal aspect correction */

static void patch_hud_fix(uintptr_t base, float hud_x_scale) {
    /* scan .rdata zero padding for 32 consecutive 0x00 bytes */
    uintptr_t cave = 0;
    for (uintptr_t scan = base + 0x22d8f000u; scan <= base + 0x22d90000u - 32; scan += 4) {
        int all_zero = 1;
        const uint8_t* p = (const uint8_t*)scan;
        for (int i = 0; i < 32; i++) {
            if (p[i] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            cave = scan;
            break;
        }
    }

    if (!cave) {
        log_raw("[-] hud_fix: failed to find 32-byte zero cave in .rdata");
        g_failures++;
        return;
    }

    DWORD old = 0;
    if (!VirtualProtect((LPVOID)cave, 32, PAGE_EXECUTE_READWRITE, &old)) {
        log_raw("[-] hud_fix: VirtualProtect failed for cave");
        g_failures++;
        return;
    }

    float* slots = (float*)cave;
    slots[0] = 1.6f * hud_x_scale;
    slots[1] = 0.2f * hud_x_scale;
    slots[2] = 0.125f * hud_x_scale;
    slots[3] = 0.2375f * hud_x_scale;
    VirtualProtect((LPVOID)cave, 32, old, &old);

    /* 5 RIP-relative instructions in Drawing HUD elements */
    struct {
        uintptr_t rva;
        int len;
        int disp_off;
        int slot_idx;
    } hud_targets[] = {
        { 0x22c5e955u, 9, 5, 0 }, /* 1.6f */
        { 0x22c5edfbu, 8, 4, 0 }, /* 1.6f */
        { 0x22c5eadcu, 8, 4, 1 }, /* 0.2f */
        { 0x22c5eb96u, 8, 4, 2 }, /* 0.125f */
        { 0x22c5eb55u, 8, 4, 3 }  /* 0.2375f */
    };

    int redirected = 0;
    for (int i = 0; i < 5; i++) {
        uintptr_t instr_addr = base + hud_targets[i].rva;
        uintptr_t disp_addr = instr_addr + hud_targets[i].disp_off;
        uintptr_t target_float = (uintptr_t)&slots[hud_targets[i].slot_idx];
        int32_t new_disp = (int32_t)(target_float - (instr_addr + hud_targets[i].len));

        if (!VirtualProtect((LPVOID)disp_addr, 4, PAGE_EXECUTE_READWRITE, &old)) {
            g_failures++;
            continue;
        }
        *(int32_t*)disp_addr = new_disp;
        VirtualProtect((LPVOID)disp_addr, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)instr_addr, hud_targets[i].len);
        redirected++;
    }

    if (redirected == 5) {
        char buf[128], s_scale[32];
        fmt_float(hud_x_scale, 5, s_scale, sizeof(s_scale));
        my_format(buf, sizeof(buf), "[ OK ] hud_fix: 5 HUD width instructions redirected (scale=%s)", s_scale);
        log_raw(buf);
    } else {
        char buf[128];
        my_format(buf, sizeof(buf), "[-] hud_fix: only %d/5 instructions redirected", redirected);
        log_raw(buf);
        g_failures++;
    }
}

static void write_default_ini(void) {
    static const char kIni[] =
        "; spelunky 2 native 4:3\n"
        "[aspect]\n"
        "aspect_ratio = 1.3333334\n"
        "world_projection = 1\n"
        "no_letterbox = 1\n"
        "cam_extents = 1\n"
        "\n"
        "; the ui canvas clamp at 0x22ce40db compares against this constant.\n"
        "; leaving it at 16:9 forces the 2d canvas into a 16:9 box -> menus get\n"
        "; vertically stretched and the bottom bar falls off screen.\n"
        "aspect_globals = 1\n"
        "proj_constants = 0\n"
        "aspect_vectors = 0\n"
        "runtime_immediates = 0\n"
        "\n"
        "; ui_quads stays off: a device run showed it changes nothing on screen (the\n"
        "; rects it patches are entity / world data, not hud geometry).\n"
        "ui_quads = 0\n"
        "ui_scale_x = 1.0\n"
        "ui_scale_y = 1.0\n"
        "\n"
        "; surgical hud width redirect. never validated, keep it off.\n"
        "hud_fix = 0\n"
        "hud_x_scale = 1.0\n"
        "\n"
        "; the whole 2d ui batch is drawn under a single viewport built from the\n"
        "; ui canvas, which we widen to the 4:3 window for the world's sake. the\n"
        "; ui geometry is 16:9-authored, so that leaves every quad 0.75 narrow.\n"
        "; 1 = keep the 2d ui in a centred 16:9 band; 0 = stretch it over 4:3.\n"
        "; device run 95818d0: the band fixed the menu text but letterboxed every\n"
        "; later pass (the viewport leaks out of the hud draw) and never reached\n"
        "; the in-level hud icons at all.\n"
        "ui_letterbox = 0\n"
        "\n"
        "; every 2d element is authored for a 1280x720 frame and the canvas is now\n"
        "; 1280x960, so the whole 2d layer comes out 1.3333 too tall. 1 = scale y\n"
        "; by 720/960 = 0.75 at the one 2d draw call, so proportions come out square.\n"
        "; mutually exclusive with ui_letterbox - both fix the same axis.\n"
        "ui_2d_scale = 1\n"
        "ui_2d_scale_x = 1.0\n"
        "; a cluster whose y box spans the frame from -edge to +edge is a fade /\n"
        "; dim / blur fill and is left at full height so it keeps covering the\n"
        "; window; every other cluster scales even when it overshoots the frame\n"
        "; mid-zoom.\n"
        "ui_2d_edge = 0.995\n"
        "; 1 = decide fills per element: one at least 1.8 tall that touches an\n"
        "; edge is a fade / dim / blur and keeps its full height. 0 = the old\n"
        "; per-vertex line (vertices past ui_2d_edge are left where they are),\n"
        "; which shears art crossing that line but leaves fills alone.\n"
        "ui_2d_fill = 1\n"
        "; 1 = split equal-height backing bars stacked at partial overlap into\n"
        "; separate elements. experimental, off.\n"
        "ui_2d_bars = 0\n"
        "; 1 = clusters on the centre line scale about y = 0 instead of their own\n"
        "; centre, so pause / confirm text follows its shrunk board.\n"
        "ui_2d_center = 1\n"
        "; heap scan for stray copies of the projection matrix. never matched, off.\n"
        "live_scan = 0\n"
        "; the layer is scaled per element, not about the screen centre: quads whose\n"
        "; centres sit within ui_2d_cluster of each other in y are one element. a\n"
        "; batch pins to the edge it hugs only when every element in it is compact\n"
        "; (<= ui_2d_pinspan), past ui_2d_pin and out in a corner past ui_2d_pinx;\n"
        "; mixed batches are art and scale each element about its own centre, so no\n"
        "; authored position moves. the corner test keeps the camp rope's top\n"
        "; segments (compact, past 0.85, dead centre) from pinning and breaking.\n"
        "ui_2d_pin = 0.85\n"
        "ui_2d_pinspan = 0.35\n"
        "ui_2d_cluster = 0.20\n"
        "ui_2d_pinx = 0.30\n"
        "; a glyph-sized quad (<= 0.12 tall, <= 0.06 wide) sitting further than\n"
        "; ui_2d_rowgap from the run so far starts a new element even when the\n"
        "; centres are within ui_2d_cluster, so stacked text lines keep their\n"
        "; authored row positions instead of being squeezed towards the block\n"
        "; centre. only lines split: the run must already be 0.06 wide and 6 quads\n"
        "; long, so art pieces and the camp rope (0.03 wide) never do. 0 disables.\n"
        "ui_2d_rowgap = 0.05\n"
        "; 1 = log every distinct 2d batch shape once (n, idx, x/y/z range, quad\n"
        "; centres). set to 0 once the layout questions are answered.\n"
        "ui_2d_dump = 0\n"
        "\n"
        "; 13.5 = vanilla zoom = 7.2 tiles tall, fits a room. >= 15.0 is a full\n"
        "; 8 tile room and taller values break the camera clamp (level entry jitter).\n"
        "custom_zoom = 13.5\n"
        "\n"
        "; android's 39-bit address space can't satisfy the game's heap hint, so it\n"
        "; hangs re-rolling it. 1 = shrink the hint range (two one-byte patches).\n"
        "heap_patch = 1\n"
        "\n"
        "; the game mutes the sfx / music buses on the device and never restores them.\n"
        "; 1 = push the local.cfg volumes back onto the buses for the first minute.\n"
        "force_audio = 1\n"
        "debug_log = 0\n";

    HANDLE h = CreateFileA(g_ini_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, kIni, (DWORD)(sizeof(kIni) - 1), &wrote, NULL);
    CloseHandle(h);
}

static int read_whole_file(const char* path, char* out, int max) {
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    if (!ReadFile(h, out, (DWORD)max - 1, &got, NULL)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    out[got] = '\0';
    return 1;
}

static int ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + my_strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = '\0';
    return s;
}

static int ini_get(const char* section, const char* key,
                   char* out, int outsz, const char* def) {
    static char buf[4096];
    lstrcpynA(out, def, outsz);
    if (!read_whole_file(g_ini_path, buf, (int)sizeof(buf))) return 0;

    char* cur = buf;
    int in_section = 0;
    while (*cur) {
        char* line = cur;
        char* nl = cur;
        while (*nl && *nl != '\n') nl++;
        if (*nl) { *nl = '\0'; cur = nl + 1; } else cur = nl;

        char* p = trim(line);
        if (*p == '[') {
            char* close = p;
            while (*close && *close != ']') close++;
            if (!*close) continue;
            *close = '\0';
            in_section = ieq(trim(p + 1), section);
            continue;
        }
        if (!in_section) continue;
        if (!*p || *p == ';' || *p == '#') continue;

        char* eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        if (!ieq(trim(p), key)) continue;

        lstrcpynA(out, trim(eq + 1), outsz);
        return 1;
    }
    return 0;
}

static int ini_get_int(const char* section, const char* key, int def) {
    char buf[64];
    char def_str[16];
    fmt_int(def, def_str, sizeof(def_str));
    if (!ini_get(section, key, buf, (int)sizeof(buf), def_str)) {
        return def;
    }
    char* p = buf;
    while (*p == ' ' || *p == '\t') p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;
    if (*p < '0' || *p > '9') return def;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return sign * v;
}

static float parse_float(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    float sign = 1.0f;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1.0f; s++; }

    float value = 0.0f;
    while (*s >= '0' && *s <= '9') { value = value * 10.0f + (float)(*s - '0'); s++; }

    if (*s == '.') {
        s++;
        float scale = 0.1f;
        while (*s >= '0' && *s <= '9') { value += scale * (float)(*s - '0'); scale *= 0.1f; s++; }
    }
    return sign * value;
}

static void read_ini(void) {
    char buf[64];

    ini_get("aspect", "aspect_ratio", buf, (int)sizeof(buf), "1.3333334");
    float target_aspect = parse_float(buf);
    if (target_aspect < 1.0f || target_aspect > 2.5f) target_aspect = DEFAULT_ASPECT;
    g_aspect = target_aspect;

    ini_get("aspect", "custom_zoom", buf, (int)sizeof(buf), "13.5");
    g_custom_zoom = parse_float(buf);
    if (g_custom_zoom < 0.0f) g_custom_zoom = 0.0f;

    g_debug = ini_get_int("aspect", "debug_log", 0) != 0;

    g_do_world       = ini_get_int("aspect", "world_projection", 1) != 0;
    g_do_noletterbox = ini_get_int("aspect", "no_letterbox", 1) != 0;
    g_do_camext      = ini_get_int("aspect", "cam_extents", 1) != 0;

    g_do_hud_fix     = ini_get_int("aspect", "hud_fix", 0) != 0;
    ini_get("aspect", "hud_x_scale", buf, (int)sizeof(buf), "1.0");
    g_hud_x_scale = parse_float(buf);
    if (g_hud_x_scale < 0.1f || g_hud_x_scale > 10.0f) g_hud_x_scale = 1.0f;

    // Granular flags (default to 0)
    g_do_aspect_globals       = ini_get_int("aspect", "aspect_globals", 1) != 0;
    g_do_proj_constants       = ini_get_int("aspect", "proj_constants", 0) != 0;
    g_do_aspect_vectors       = ini_get_int("aspect", "aspect_vectors", 0) != 0;
    g_do_runtime_immediates   = ini_get_int("aspect", "runtime_immediates", 0) != 0;
    g_do_ui_quads             = ini_get_int("aspect", "ui_quads", 0) != 0;
    g_do_ui_letterbox         = ini_get_int("aspect", "ui_letterbox", 0) != 0;
    g_do_ui_2d                = ini_get_int("aspect", "ui_2d_scale", 1) != 0;

    ini_get("aspect", "ui_2d_scale_x", buf, (int)sizeof(buf), "1.0");
    g_ui_2d_scale_x = parse_float(buf);
    if (g_ui_2d_scale_x < 0.5f || g_ui_2d_scale_x > 3.0f) g_ui_2d_scale_x = 1.0f;

    ini_get("aspect", "ui_2d_edge", buf, (int)sizeof(buf), "0.995");
    g_ui_2d_edge = parse_float(buf);
    if (g_ui_2d_edge < 0.90f || g_ui_2d_edge > 1.0f) g_ui_2d_edge = 0.995f;

    ini_get("aspect", "ui_2d_pin", buf, (int)sizeof(buf), "0.85");
    g_ui_2d_pin = parse_float(buf);
    if (g_ui_2d_pin < 0.0f || g_ui_2d_pin > 1.0f) g_ui_2d_pin = 0.85f;

    ini_get("aspect", "ui_2d_pinspan", buf, (int)sizeof(buf), "0.35");
    g_ui_2d_pinspan = parse_float(buf);
    if (g_ui_2d_pinspan < 0.05f || g_ui_2d_pinspan > 4.0f) g_ui_2d_pinspan = 0.35f;

    ini_get("aspect", "ui_2d_cluster", buf, (int)sizeof(buf), "0.20");
    g_ui_2d_cluster = parse_float(buf);
    if (g_ui_2d_cluster < 0.0f || g_ui_2d_cluster > 1.0f) g_ui_2d_cluster = 0.20f;

    ini_get("aspect", "ui_2d_pinx", buf, (int)sizeof(buf), "0.30");
    g_ui_2d_pinx = parse_float(buf);
    if (g_ui_2d_pinx < 0.0f || g_ui_2d_pinx > 2.0f) g_ui_2d_pinx = 0.30f;

    ini_get("aspect", "ui_2d_rowgap", buf, (int)sizeof(buf), "0.05");
    g_ui_2d_rowgap = parse_float(buf);
    if (g_ui_2d_rowgap < 0.0f || g_ui_2d_rowgap > 0.5f) g_ui_2d_rowgap = 0.05f;

    g_ui_2d_fill = ini_get_int("aspect", "ui_2d_fill", 1) != 0;
    g_ui_2d_bars = ini_get_int("aspect", "ui_2d_bars", 0) != 0;
    g_ui_2d_center = ini_get_int("aspect", "ui_2d_center", 1) != 0;
    g_do_live_scan = ini_get_int("aspect", "live_scan", 0) != 0;
    g_do_heap_patch = ini_get_int("aspect", "heap_patch", 1) != 0;
    g_do_force_audio = ini_get_int("aspect", "force_audio", 1) != 0;

    g_do_ui_2d_dump = ini_get_int("aspect", "ui_2d_dump", 0) != 0;

    ini_get("aspect", "ui_scale_x", buf, (int)sizeof(buf), "1.0");
    g_ui_scale_x = parse_float(buf);
    if (g_ui_scale_x < 0.5f || g_ui_scale_x > 3.0f) g_ui_scale_x = 1.0f;

    ini_get("aspect", "ui_scale_y", buf, (int)sizeof(buf), "1.0");
    g_ui_scale_y = parse_float(buf);
    if (g_ui_scale_y < 0.5f || g_ui_scale_y > 3.0f) g_ui_scale_y = 1.0f;
}

/* ### audio workaround
 *
 * on the device the game mutes Master_SFX / Master_BGM (setMute + setVolume 0)
 * and never restores them, so there is no sound. the fix is to learn each bus
 * pointer as the game fetches it (System::getBus is the one call that sees the
 * name and the opaque pointer together, and it runs before FMOD init, so it has
 * to be hooked from DllMain), then push the local.cfg volumes back every few
 * seconds for a minute. setVolume / setMute need no hook: their IAT slots are
 * read and called directly. all three are x64 member functions. */

static int my_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static const char* my_strstr(const char* hay, const char* needle) {
    size_t n = my_strlen(needle);
    if (!n) return hay;
    for (; *hay; hay++)
        if (*hay == *needle && my_memcmp(hay, needle, n) == 0) return hay;
    return NULL;
}

/* walk the exe's imports and, for every named import matching `needle`, either
 * swap the slot for `proxy` (saving the original in *real_out) or, when proxy is
 * NULL, just read the slot into *real_out.
 *
 * exact=1 demands a whole-name match, exact=0 a substring. exact matters for
 * prefix-sharing names (VirtualAlloc is a prefix of VirtualAllocEx, and a
 * substring match there hooks the wrong slot and recurses). FMOD's names are
 * mangled with decorations that vary, so those use substring. returns the
 * number of slots that matched. */
static int patch_one_import(HMODULE mod, const char* needle, int exact,
                            void* proxy, void** real_out) {
    int hits = 0;
    if (!mod) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return 0;

    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((uint8_t*)mod + imp_rva);
    for (; imp->Name; imp++) {
        if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;
        PIMAGE_THUNK_DATA orig = (PIMAGE_THUNK_DATA)((uint8_t*)mod + imp->OriginalFirstThunk);
        PIMAGE_THUNK_DATA iat  = (PIMAGE_THUNK_DATA)((uint8_t*)mod + imp->FirstThunk);
        for (; orig->u1.AddressOfData; orig++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)((uint8_t*)mod + orig->u1.AddressOfData);
            int match = exact ? (my_strcmp((const char*)ibn->Name, needle) == 0)
                              : (my_strstr((const char*)ibn->Name, needle) != NULL);
            if (!match) continue;
            hits++;
            if (!iat->u1.Function) continue;
            if (!proxy) {
                if (real_out && !*real_out) *real_out = (void*)iat->u1.Function;
                continue;
            }
            if (iat->u1.Function == (ULONG_PTR)proxy) continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) continue;
            if (real_out && !*real_out) *real_out = (void*)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)proxy;
            DWORD ignored;
            VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), old, &ignored);
        }
    }
    return hits;
}

typedef int (*PFN_StudioGetBus)(const void*, const char*, void**);
typedef int (*PFN_BusSetVolume)(void*, float);
typedef int (*PFN_BusSetMute)(void*, int);

static PFN_StudioGetBus g_real_getbus    = NULL;
static PFN_BusSetVolume g_real_setvolume = NULL;
static PFN_BusSetMute   g_real_setmute   = NULL;

#define BUS_SLOTS 32
static struct { void* volatile ptr; char name[64]; } g_bus[BUS_SLOTS];

static int hooked_getbus(const void* self, const char* path, void** out) {
    if (!g_real_getbus) return 0;
    if (out) *out = NULL;
    int hr = g_real_getbus(self, path, out);
    void* b = out ? *out : NULL;
    if (hr == 0 && b && path) {
        for (int i = 0; i < BUS_SLOTS; i++) {
            if (g_bus[i].ptr == b) break;
            if (!g_bus[i].ptr) {
                lstrcpynA(g_bus[i].name, path, (int)sizeof(g_bus[i].name));
                g_bus[i].ptr = b; /* published last, the audio thread keys off it */
                break;
            }
        }
    }
    return hr;
}

static void install_audio_hooks(void) {
    HMODULE exe = GetModuleHandleA(NULL);
    int e = patch_one_import(exe, "?getBus@System@Studio@FMOD@@", 0,
                             (void*)hooked_getbus, (void**)&g_real_getbus);
    int c = patch_one_import(exe, "?setVolume@Bus@Studio@FMOD@@", 0,
                             NULL, (void**)&g_real_setvolume);
    int d = patch_one_import(exe, "?setMute@Bus@Studio@FMOD@@", 0,
                             NULL, (void**)&g_real_setmute);
    char b[96];
    my_format(b, sizeof(b), "[audio] hooks getBus=%d setVolume=%d setMute=%d (slots matched)", e, c, d);
    log_raw(b);
}

/* value of "<tag>N" in a local.cfg buffer, or def when absent */
static int cfg_int(const char* cfg, const char* tag, int def) {
    const char* p = my_strstr(cfg, tag);
    if (!p) return def;
    p += my_strlen(tag);
    while (*p == ' ' || *p == '\t') p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (*p < '0' || *p > '9') return def;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return sign * v;
}

static DWORD WINAPI audio_thread(LPVOID unused) {
    (void)unused;
    static char cfg[16384];
    char cfg_path[MAX_PATH];
    get_exe_dir_file(cfg_path, sizeof(cfg_path), "local.cfg");
    int last_named = -1;
    float vm = 1.6f, vs = 0.2f, vb = 0.16f;
    for (int tick = 0; tick < 20; tick++) {
        Sleep(3000);
        /* the game's own mapping is cfg value / 50 (master 80 -> 1.6) */
        int en_m = 1, en_s = 1, en_b = 1;
        cfg[0] = '\0';
        if (read_whole_file(cfg_path, cfg, (int)sizeof(cfg))) {
            vm = (float)cfg_int(cfg, "<master_volume>", 80) / 50.0f;
            vs = (float)cfg_int(cfg, "<sound_volume>", 10) / 50.0f;
            vb = (float)cfg_int(cfg, "<music_volume>", 8) / 50.0f;
            en_m = cfg_int(cfg, "<master_enabled>", 1) != 0;
            en_s = cfg_int(cfg, "<sound_enabled>", 1) != 0;
            en_b = cfg_int(cfg, "<music_enabled>", 1) != 0;
        }
        int named = 0;
        for (int i = 0; i < BUS_SLOTS; i++) {
            void* b = g_bus[i].ptr;
            if (!b) continue;
            named++;
            if (!g_real_setvolume || !g_real_setmute) continue;
            float t;
            int en;
            if (lstrcmpiA(g_bus[i].name, "bus:/Master_SUM") == 0)                { t = vm; en = en_m; }
            else if (lstrcmpiA(g_bus[i].name, "bus:/Master_SUM/Master_SFX") == 0) { t = vs; en = en_s; }
            else if (lstrcmpiA(g_bus[i].name, "bus:/Master_SUM/Master_BGM") == 0) { t = vb; en = en_b; }
            else continue;
            if (!en) continue; /* disabled in the settings: stays as the game left it, muted */
            g_real_setvolume(b, t);
            g_real_setmute(b, 0);
        }
        if (tick == 0 || tick == 19 || named != last_named) {
            char line[128], a[32], b[32], c[32];
            fmt_float(vm, 3, a, sizeof(a));
            fmt_float(vs, 3, b, sizeof(b));
            fmt_float(vb, 3, c, sizeof(c));
            my_format(line, sizeof(line), "[audio] t+%ds buses=%d master=%s sfx=%s bgm=%s",
                      (tick + 1) * 3, named, a, b, c);
            log_raw(line);
            last_named = named;
        }
    }
    return 0;
}

static void apply_patches(void) {
    static LONG claimed = 0;
    if (InterlockedCompareExchange(&claimed, 1, 0) != 0) return;

    crumb("[crumb] apply_patches entered");
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    if (!g_base) { crumb("[crumb] no base, aborting"); return; }

    get_exe_dir_file(g_ini_path, sizeof(g_ini_path), INI_NAME);
    crumb("[crumb] ini path resolved");

    if (GetFileAttributesA(g_ini_path) == INVALID_FILE_ATTRIBUTES) write_default_ini();
    crumb("[crumb] ini ready");

    read_ini();
    crumb("[crumb] ini read");

    init_log_paths();
    if (g_debug) DeleteFileA(g_log_path);

    float target_aspect = g_aspect;
    float inv_target = 1.0f / target_aspect;

    char toggle[512], s_ar[32], s_zoom[32], s_hexbase[32], s_hud[32], s_uix[32], s_uiy[32], s_u2x[32], s_u2e[32], s_u2p[32], s_u2c[32], s_u2px[32], s_u2rg[32];
    fmt_float(target_aspect, 5, s_ar, sizeof(s_ar));
    fmt_float(g_custom_zoom, 2, s_zoom, sizeof(s_zoom));
    fmt_float(g_hud_x_scale, 5, s_hud, sizeof(s_hud));
    fmt_float(g_ui_scale_x, 5, s_uix, sizeof(s_uix));
    fmt_float(g_ui_scale_y, 5, s_uiy, sizeof(s_uiy));
    fmt_float(g_ui_2d_scale_x, 5, s_u2x, sizeof(s_u2x));
    fmt_float(g_ui_2d_edge, 5, s_u2e, sizeof(s_u2e));
    fmt_float(g_ui_2d_pin, 3, s_u2p, sizeof(s_u2p));
    fmt_float(g_ui_2d_cluster, 3, s_u2c, sizeof(s_u2c));
    fmt_float(g_ui_2d_pinx, 3, s_u2px, sizeof(s_u2px));
    fmt_float(g_ui_2d_rowgap, 3, s_u2rg, sizeof(s_u2rg));
    fmt_hex64(g_base, s_hexbase, sizeof(s_hexbase));

    my_format(toggle, sizeof(toggle), "[*] ar=%s world=%d no_lb=%d cam=%d quads=%d(x%s y%s) hud=%d(x%s) uilb=%d ui2d=%d(x%s edge%s fill%d bars%d ctr%d pin%s cl%s px%s rg%s) (gl=%d pc=%d vec=%d ri=%d) zoom=%s heap=%d audio=%d dbg=%d",
              s_ar, g_do_world, g_do_noletterbox, g_do_camext,
              g_do_ui_quads, s_uix, s_uiy, g_do_hud_fix, s_hud, g_do_ui_letterbox,
              g_do_ui_2d, s_u2x, s_u2e, g_ui_2d_fill, g_ui_2d_bars, g_ui_2d_center, s_u2p, s_u2c, s_u2px, s_u2rg,
              g_do_aspect_globals, g_do_proj_constants, g_do_aspect_vectors, g_do_runtime_immediates,
              s_zoom, g_do_heap_patch, g_do_force_audio, g_debug);

    truncate_file(g_log_path);
    truncate_file(g_boot_path);

    boot_raw(g_embedded ? "[BOOT] " MOD_VERSION " (embedded in Spel2.exe)"
                        : "[BOOT] " MOD_VERSION " (freestanding build)");
    boot_raw(toggle);

    log_raw("[*] " MOD_VERSION " (freestanding build)");
    char buf[128];
    my_format(buf, sizeof(buf), "[*] base=0x%s", s_hexbase);
    log_raw(buf);
    log_raw(toggle);

    /* getBus must be hooked before FMOD initialises, so this comes first */
    if (g_do_force_audio) {
        install_audio_hooks();
        CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    } else {
        log_raw("[skip] force_audio off");
    }

    /* independent of every aspect toggle: without it android never gets past boot */
    if (g_do_heap_patch) {
        patch_heap_limit();
    } else {
        log_raw("[skip] heap_patch off");
    }

    /* 1. world projection */
    if (g_do_world) {
        float m11 = 0.0f;
        if (!safe_read_float(g_base + RVA_PROJ_M11, &m11) || m11 < 1.0f || m11 > 8.0f) {
            log_raw("[FAIL] proj_m11 unreadable");
            g_failures++;
        } else {
            char s_m11[32], s_m00[32];
            fmt_float(m11, 5, s_m11, sizeof(s_m11));
            fmt_float(m11 * inv_target, 5, s_m00, sizeof(s_m00));
            my_format(buf, sizeof(buf), "[*] m11=%s m00:=%s", s_m11, s_m00);
            log_raw(buf);
            patch_float(g_base + RVA_PROJ_M00, 1.3579950f, m11 * inv_target,
                        m11 * inv_target, "proj_m00");
        }
    } else {
        log_raw("[skip] world_projection off");
    }

    /* 2. letterbox bypass:
     * A) In D3D11 viewport setup at 0x22cc1966 & 0x22cc197f:
     *    Force full un-letterboxed viewport dimensions (w=1280, h=960, x=0, y=0)
     *    by jumping directly to full size:
     *    0x22cc1966: 76 0e -> eb 11 (jmp to 0x22cc1979: mov r14d, ecx; mov r15d, eax)
     *    0x22cc197f: 76 0c -> eb 0c (jmp to 0x22cc198d)
     * B) In UI canvas setup at 0x22ce40bc & 0x22ce41fa:
     *    Force full un-letterboxed UI canvas bounds (1280x960):
     *    0x22ce40bc: 3c 02 74 53 -> eb 55 90 90 (jmp to 0x22ce4113)
     *    0x22ce41fa: 76 0d -> eb 33 (jmp to 0x22ce422f)
     */
    if (g_do_noletterbox) {
        static const unsigned char jbe1_orig[] = { 0x76, 0x0E };
        static const unsigned char jbe1_repl[] = { 0xEB, 0x11 };
        patch_bytes(g_base + 0x22cc1966u, jbe1_orig, jbe1_repl, 2, "viewport_full_w");

        static const unsigned char jbe2_orig[] = { 0x76, 0x0C };
        static const unsigned char jbe2_repl[] = { 0xEB, 0x0C };
        patch_bytes(g_base + 0x22cc197fu, jbe2_orig, jbe2_repl, 2, "viewport_full_h");

        static const unsigned char ui_orig[] = { 0x3C, 0x02, 0x74, 0x53 };
        static const unsigned char ui_repl[] = { 0xEB, 0x55, 0x90, 0x90 };
        patch_bytes(g_base + 0x22ce40bcu, ui_orig, ui_repl, 4, "ui_canvas_full");

        static const unsigned char scis_orig[] = { 0x76, 0x0D };
        static const unsigned char scis_repl[] = { 0xEB, 0x33 };
        patch_bytes(g_base + 0x22ce41fau, scis_orig, scis_repl, 2, "ui_scissor_full");

        /* C) Menu aspect unscale removed */
    } else {
        log_raw("[skip] no_letterbox off");
    }

    /* 3. camera extents */
    if (g_do_camext) {
        log_raw("[skip] cam_extents off (prevents camera rope jitter)");
    } else {
        log_raw("[skip] cam_extents off");
    }

    /* 4. granular aspect controls */
    if (g_do_aspect_globals) {
        log_raw("[skip] aspect_globals off (preserves full menu framing)");
    } else {
        log_raw("[skip] aspect_globals off");
    }

    if (g_do_proj_constants) {
        patch_float(g_base + RVA_PROJ_INV_NEG, -0.5625f,    -0.75f,    -inv_target,                "proj_inv_neg");
        patch_float(g_base + RVA_PROJ_10134,    1.0666667f,  0.8f,      0.6f * target_aspect,       "proj_10134");
        patch_float(g_base + RVA_PROJ_1013C,    1.8962963f,  1.0666667f, 0.6f * target_aspect * target_aspect, "proj_1013c");
        patch_float(g_base + RVA_PROJ_10148,   -0.0675f,    -0.09f,    -0.12f * inv_target,         "proj_10148_neg");
        patch_float(g_base + RVA_PROJ_10160,    0.0675f,     0.09f,     0.12f * inv_target,         "proj_10160_pos");
        patch_float(g_base + RVA_PROJ_10180,    0.9481481f,  0.7111112f, (8.0f / 15.0f) * target_aspect, "proj_10180");
        patch_float(g_base + RVA_PROJ_10184,    0.478125f,   0.6375f,    0.85f * inv_target,         "proj_10184_pos");
        patch_float(g_base + RVA_PROJ_10188,   -0.478125f,  -0.6375f,   -0.85f * inv_target,         "proj_10188_neg");
    } else {
        log_raw("[skip] proj_constants off");
    }

    if (g_do_aspect_vectors) {
        static const struct { uintptr_t rva; const char* name; } aspect_vecs[] = {
            { 0x22d0e320u, "vec_e320_camera"    },
            { 0x22d0e460u, "vec_e460_camera"    },
            { 0x22d0f890u, "vec_f890_camera"    },
            { 0x22d0f990u, "vec_f990_roomscale" },
            { 0x22d0ff70u, "vec_ff70_level"     },
            { 0x22d0fff0u, "vec_fff0_level"     },
            { 0x22d10020u, "vec_10020_level"    },
            { 0x22d10170u, "vec_10170_PROJECT"  },
            { 0x22d10ba0u, "vec_10ba0_level"    },
            { 0x22d11860u, "vec_11860_render"    },
        };
        int n = (int)(sizeof(aspect_vecs) / sizeof(aspect_vecs[0]));
        int vec_ok = 0;
        for (int i = 0; i < n; i++)
            vec_ok += patch_vec4(g_base + aspect_vecs[i].rva, target_aspect, aspect_vecs[i].name);
        char bvec[64];
        my_format(bvec, sizeof(bvec), "[*] aspect vectors: %d/%d", vec_ok, n);
        log_raw(bvec);
    } else {
        log_raw("[skip] aspect_vectors off");
    }

    if (g_do_runtime_immediates) {
        patch_runtime_aspect(g_base, target_aspect);
    } else {
        log_raw("[skip] runtime_immediates off");
    }

    if (g_do_ui_quads) {
        patch_ui_quads(g_base, g_ui_scale_x, g_ui_scale_y);
    } else {
        log_raw("[skip] ui_quads off");
    }

    /* hud_fix disabled by default */
    if (g_do_hud_fix) {
        patch_hud_fix(g_base, g_hud_x_scale);
    } else {
        log_raw("[skip] hud_fix off");
    }

    log_raw(g_do_live_scan ? "[*] live_scan on (heap m00 scan, fault-safe reads)"
                           : "[skip] live_scan off (heap m00 scan never matched; it crashed pin10)");

    /* the 2d ui is authored for a 16:9 frame; either keep it in a band
     * (ui_letterbox) or un-stretch it where it is drawn (ui_2d, the default). */
    if (g_do_ui_letterbox) {
        patch_ui_letterbox(g_base);
    } else {
        log_raw("[skip] ui_letterbox off (using ui_2d_scale instead)");
    }

    if (g_do_ui_2d) {
        patch_ui_2d(g_base);
    } else {
        log_raw("[skip] ui_2d_scale off (2d ui stretched across the 4:3 window)");
    }

    if (g_custom_zoom > 1.0f) {
        float shop_zoom = g_custom_zoom * (12.5f / 13.5f);
        patch_float(g_base + RVA_ZOOM_DEFAULT, 13.5f, 13.5f, g_custom_zoom, "zoom_default");
        patch_float(g_base + RVA_ZOOM_SHOP,    12.5f, 12.5f, shop_zoom,  "zoom_shop");
        patch_float(g_base + RVA_ZOOM_CODE1,   13.5f, 13.5f, g_custom_zoom, "zoom_code1");
        patch_float(g_base + RVA_ZOOM_CODE2,   13.5f, 13.5f, g_custom_zoom, "zoom_code2");
    }

    if (g_failures == 0) {
        log_raw("[+] SUCCESS: all patches applied and verified");
        boot_raw("[BOOT] RESULT: SUCCESS, 0 failures");
    } else {
        char bres[64];
        my_format(bres, sizeof(bres), "[-] FAILED: %d patch(es) failed", g_failures);
        log_raw(bres);
        boot_raw(bres);
    }

    /* the watchdog holds m00 and always ran on the validated build; only its
     * log line is debug-gated. the matrix poll is diagnostics only. */
    if (g_debug) CreateThread(NULL, 0, matrix_report_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
}

/* the game calls this expecting the discord sdk. returning failure reads as
 * "discord not running", and it carries on without it. */
__declspec(dllexport) int DiscordCreate(int version, void* params, void** result) {
    (void)version; (void)params;
    if (result) *result = NULL;
    return 1;
}

/* ### embedded mode
 *
 * the web patcher carries this same dll inside Spel2.exe instead of next to
 * it: it lays the image out in a new exe section named EMBED_SECTION and
 * points the exe's entry at a stub that calls embedded_entry(exe, image),
 * then jumps on to the real entry. no loader has touched the image, so this
 * relocates it and binds its kernel32 imports itself. until that is done it
 * must not call an import or read a global that holds an address: the two
 * functions it needs come from the exe's own, already bound, import table. */

#define EMBED_SECTION ".m43"

/* 1 when the exe carries the embedded mod. a dll copy loaded next to such an
 * exe stands down, so the two never patch the same bytes. */
static int exe_has_embed(void) {
    uint8_t* exe = (uint8_t*)GetModuleHandleA(NULL);
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(exe + ((PIMAGE_DOS_HEADER)exe)->e_lfanew);
    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (my_memcmp(s->Name, EMBED_SECTION, sizeof(EMBED_SECTION)) == 0) return 1;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved);

__declspec(dllexport) void embedded_entry(uint8_t* exe, uint8_t* img) {
    FARPROC (WINAPI *gpa)(HMODULE, LPCSTR) = NULL;
    HMODULE (WINAPI *gmh)(LPCSTR) = NULL;
    patch_one_import((HMODULE)exe, "GetProcAddress", 1, NULL, (void**)&gpa);
    patch_one_import((HMODULE)exe, "GetModuleHandleA", 1, NULL, (void**)&gmh);
    if (!gpa || !gmh) return;

    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(img + ((PIMAGE_DOS_HEADER)img)->e_lfanew);
    PIMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory;

    uint64_t delta = (uint64_t)(uintptr_t)img - nt->OptionalHeader.ImageBase;
    uint8_t* r = img + dir[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    uint8_t* r_end = r + dir[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
    while (r < r_end) {
        PIMAGE_BASE_RELOCATION b = (PIMAGE_BASE_RELOCATION)r;
        if (b->SizeOfBlock < sizeof(*b)) break;
        for (WORD* e = (WORD*)(b + 1); (uint8_t*)e < r + b->SizeOfBlock; e++)
            if ((*e >> 12) == IMAGE_REL_BASED_DIR64)
                *(uint64_t*)(img + b->VirtualAddress + (*e & 0xfff)) += delta;
        r += b->SizeOfBlock;
    }

    PIMAGE_IMPORT_DESCRIPTOR imp =
        (PIMAGE_IMPORT_DESCRIPTOR)(img + dir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        HMODULE mod = gmh((const char*)(img + imp->Name));
        if (!mod) return;
        PIMAGE_THUNK_DATA orig = (PIMAGE_THUNK_DATA)(img + imp->OriginalFirstThunk);
        PIMAGE_THUNK_DATA iat  = (PIMAGE_THUNK_DATA)(img + imp->FirstThunk);
        for (; orig->u1.AddressOfData; orig++, iat++) {
            PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)(img + orig->u1.AddressOfData);
            FARPROC fn = gpa(mod, (const char*)ibn->Name);
            if (!fn) return;
            iat->u1.Function = (ULONG_PTR)fn;
        }
    }

    /* the loader would register these; without them nothing can unwind
     * through the mod's frames */
    PIMAGE_DATA_DIRECTORY pd = &dir[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (pd->Size)
        RtlAddFunctionTable((PRUNTIME_FUNCTION)(img + pd->VirtualAddress),
                            pd->Size / sizeof(RUNTIME_FUNCTION), (DWORD64)(uintptr_t)img);

    g_embedded = 1;
    DllMain((HINSTANCE)img, DLL_PROCESS_ATTACH, NULL);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        if (!g_embedded && exe_has_embed()) {
            crumb("[crumb] exe carries the embedded mod, dll stands down");
            return TRUE;
        }
        crumb(g_embedded ? "[crumb] embedded entry" : "[crumb] dllmain entered");
        if (!g_embedded) DisableThreadLibraryCalls(hinst);
        crumb("[crumb] calling apply_patches");
        apply_patches();
        crumb("[crumb] apply_patches returned");
    }
    return TRUE;
}
