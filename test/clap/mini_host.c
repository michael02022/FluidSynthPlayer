#define _GNU_SOURCE
/* Minimal CLAP test host for FluidSynthPlayer: loads the .clap, restores
 * a state pointing at a SoundFont (no drag & drop/clicks needed), renders
 * audio and checks:
 *   - sample-accurate note placement (silence before the note's timestamp)
 *   - a note actually sounds
 *   - Bank/Patch via host param events and via MIDI Program Change (the
 *     plugin must report the latter back as a param value event)
 *   - multitimbral mode (channel 10 drums)
 *   - state save/load round trip
 * Optionally opens the GUI window for a few seconds (visual check).
 * Usage: mini_host <plugin.clap> <soundfont.sf2> [gui] [seconds]
 * Adapted from SfzDrummer's mini_host.c (sibling project). Builds on Linux
 * and Windows (see the "platform" section right below). */
#include <clap/clap.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------- platform */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>

#define GUI_API CLAP_WINDOW_API_WIN32
typedef HMODULE lib_t;
static lib_t lib_open(const char* path) { return LoadLibraryA(path); }
static void* lib_sym(lib_t l, const char* name) { return (void*)GetProcAddress(l, name); }
static void lib_close(lib_t l) { FreeLibrary(l); }
static const char* lib_error(void) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "LoadLibrary failed (error %lu)", GetLastError());
    return buf;
}
/* Sleeps while keeping the (single-threaded) UI alive: on Windows the
 * plugin's editor lives on this thread and is driven by its messages. */
static void wait_ms_pumping(unsigned ms) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    Sleep(ms);
}
static int make_dir(const char* path) { return _mkdir(path); }
static int remove_dir(const char* path) { return _rmdir(path); }
/* Live OS environment (the plugin may carry its own copy of the C runtime,
 * whose getenv wouldn't see a putenv made here). */
static void set_env(const char* name, const char* value) { SetEnvironmentVariableA(name, value); }
static char* make_temp_dir(char* buf, size_t size) {
    char tmp[MAX_PATH];
    if (!GetTempPathA(sizeof(tmp), tmp)) return NULL;
    snprintf(buf, size, "%sfsp_cfg_%lu", tmp, GetCurrentProcessId());
    return make_dir(buf) == 0 ? buf : NULL;
}
/* A real host embeds the editor in a window of its own: do the same. */
static HWND create_parent_window(uint32_t w, uint32_t h) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "mini_host_parent";
    RegisterClassA(&wc);
    RECT r = {0, 0, (LONG)w, (LONG)h};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowA("mini_host_parent", "mini_host - FluidSynthPlayer", style,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#define GUI_API CLAP_WINDOW_API_X11
typedef void* lib_t;
static lib_t lib_open(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void* lib_sym(lib_t l, const char* name) { return dlsym(l, name); }
static void lib_close(lib_t l) { dlclose(l); }
static const char* lib_error(void) { return dlerror(); }
static void wait_ms_pumping(unsigned ms) { usleep(ms * 1000u); }
static int make_dir(const char* path) { return mkdir(path, 0755); }
static int remove_dir(const char* path) { return rmdir(path); }
static void set_env(const char* name, const char* value) { setenv(name, value, 1); }
static char* make_temp_dir(char* buf, size_t size) {
    snprintf(buf, size, "/tmp/fsp_cfg_XXXXXX");
    return mkdtemp(buf);
}
#endif

/* memmem() isn't available everywhere (MinGW, MSVC). */
static int contains_bytes(const void* hay, size_t hayLen, const void* needle, size_t needleLen) {
    if (needleLen == 0) return 1;
    for (size_t i = 0; i + needleLen <= hayLen; ++i)
        if (!memcmp((const char*)hay + i, needle, needleLen)) return 1;
    return 0;
}

#define BLOCK 512
#define SR 48000.0

static int g_failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            printf("  ok   ");                            \
        } else {                                          \
            printf("  FAIL ");                            \
            ++g_failures;                                 \
        }                                                 \
        printf(__VA_ARGS__);                              \
        printf("\n");                                     \
    } while (0)

/* ----------------------------------------------------------- host stubs */

static void host_request_flush(const clap_host_t* h) { (void)h; }
static void host_rescan(const clap_host_t* h, clap_param_rescan_flags f) { (void)h; (void)f; }
static void host_clear(const clap_host_t* h, clap_id id, clap_param_clear_flags f) {
    (void)h; (void)id; (void)f;
}
static const clap_host_params_t g_host_params = {
    .rescan = host_rescan, .clear = host_clear, .request_flush = host_request_flush};

static const void* host_get_extension(const clap_host_t* h, const char* id) {
    (void)h;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &g_host_params;
    return NULL;
}
static void host_request_restart(const clap_host_t* h) { (void)h; }
static void host_request_process(const clap_host_t* h) { (void)h; }
static void host_request_callback(const clap_host_t* h) { (void)h; }

/* ---------------------------------------------------------- event lists */

typedef struct {
    union {
        clap_event_header_t hdr;
        clap_event_midi_t midi;
        clap_event_param_value_t param;
        clap_event_note_t note;
        clap_event_midi_sysex_t sysex;
    } ev[16];
    uint32_t count;
} InEvents;

static uint32_t in_size(const clap_input_events_t* l) { return ((InEvents*)l->ctx)->count; }
static const clap_event_header_t* in_get(const clap_input_events_t* l, uint32_t i) {
    return &((InEvents*)l->ctx)->ev[i].hdr;
}

typedef struct {
    int paramValueSeen[64];
    double paramValue[64];
} OutEvents;

static bool out_push(const clap_output_events_t* l, const clap_event_header_t* e) {
    OutEvents* o = (OutEvents*)l->ctx;
    if (e->type == CLAP_EVENT_PARAM_VALUE) {
        const clap_event_param_value_t* pv = (const clap_event_param_value_t*)e;
        if (pv->param_id < 64) {
            o->paramValueSeen[pv->param_id] = 1;
            o->paramValue[pv->param_id] = pv->value;
        }
    }
    return true;
}

static void add_midi(InEvents* e, uint32_t time, uint8_t s, uint8_t d1, uint8_t d2) {
    clap_event_midi_t* m = &e->ev[e->count++].midi;
    memset(m, 0, sizeof(*m));
    m->header.size = sizeof(*m);
    m->header.time = time;
    m->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    m->header.type = CLAP_EVENT_MIDI;
    m->data[0] = s;
    m->data[1] = d1;
    m->data[2] = d2;
}

static void add_param(InEvents* e, uint32_t time, clap_id id, double v) {
    clap_event_param_value_t* p = &e->ev[e->count++].param;
    memset(p, 0, sizeof(*p));
    p->header.size = sizeof(*p);
    p->header.time = time;
    p->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    p->header.type = CLAP_EVENT_PARAM_VALUE;
    p->param_id = id;
    p->note_id = -1;
    p->port_index = -1;
    p->channel = -1;
    p->key = -1;
    p->value = v;
}

static void add_sysex(InEvents* e, uint32_t time, const uint8_t* data, uint32_t size) {
    clap_event_midi_sysex_t* x = &e->ev[e->count++].sysex;
    memset(x, 0, sizeof(*x));
    x->header.size = sizeof(*x);
    x->header.time = time;
    x->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    x->header.type = CLAP_EVENT_MIDI_SYSEX;
    x->buffer = data;
    x->size = size;
}

/* ------------------------------------------------------------ processing */

static float g_left[BLOCK], g_right[BLOCK];

/* Runs one block; returns its RMS. */
static double run_block(const clap_plugin_t* plug, InEvents* in, OutEvents* out) {
    float* chans[2] = {g_left, g_right};
    clap_audio_buffer_t ob = {.data32 = chans, .data64 = NULL, .channel_count = 2,
                              .latency = 0, .constant_mask = 0};
    InEvents empty = {.count = 0};
    if (!in) in = &empty;
    OutEvents dummy;
    memset(&dummy, 0, sizeof(dummy));
    if (!out) out = &dummy;
    clap_input_events_t ie = {.ctx = in, .size = in_size, .get = in_get};
    clap_output_events_t oe = {.ctx = out, .try_push = out_push};
    clap_process_t proc = {.steady_time = -1, .frames_count = BLOCK, .transport = NULL,
                           .audio_inputs = NULL, .audio_outputs = &ob,
                           .audio_inputs_count = 0, .audio_outputs_count = 1,
                           .in_events = &ie, .out_events = &oe};
    plug->process(plug, &proc);
    double sum = 0;
    for (int i = 0; i < BLOCK; ++i) sum += g_left[i] * g_left[i] + g_right[i] * g_right[i];
    return sqrt(sum / (2.0 * BLOCK));
}

static float peak_blocks(const clap_plugin_t* plug, int n) {
    float pk = 0;
    for (int b = 0; b < n; ++b) {
        run_block(plug, NULL, NULL);
        for (int i = 0; i < BLOCK; ++i) {
            if (fabsf(g_left[i]) > pk) pk = fabsf(g_left[i]);
            if (fabsf(g_right[i]) > pk) pk = fabsf(g_right[i]);
        }
    }
    return pk;
}

static double run_blocks(const clap_plugin_t* plug, int n) {
    double peak = 0;
    for (int i = 0; i < n; ++i) {
        double r = run_block(plug, NULL, NULL);
        if (r > peak) peak = r;
    }
    return peak;
}

/* --------------------------------------------------------- state stream */

typedef struct {
    uint8_t buf[1 << 16];
    uint64_t len, pos;
} MemStream;

static int64_t mem_write(const clap_ostream_t* s, const void* d, uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    if (m->len + n > sizeof(m->buf)) n = sizeof(m->buf) - m->len;
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    return (int64_t)n;
}
static int64_t mem_read(const clap_istream_t* s, void* d, uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    uint64_t left = m->len - m->pos;
    if (n > left) n = left;
    memcpy(d, m->buf + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}
static void put(MemStream* m, const void* d, uint32_t n) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    mem_write(&os, d, n);
}
static void put_u32(MemStream* m, uint32_t v) { put(m, &v, 4); }
static void put_str(MemStream* m, const char* s) {
    uint32_t len = (uint32_t)strlen(s);
    put_u32(m, len);
    put(m, s, len);
}

/* Same layout as plugin.cpp's stateSave (version 1). */
static void build_state(MemStream* m, const char* sfPath, uint32_t program) {
    memset(m, 0, sizeof(*m));
    put_u32(m, 0x4C505346);
    put_u32(m, 1);
    put_str(m, sfPath);
    put_str(m, "synth.min-note-length = 10\n");
    put_u32(m, 1); /* one explicit param: Patch, everything else default */
    put_u32(m, 2);
    double v = program;
    put(m, &v, 8);
    for (int c = 0; c < 16; ++c) {
        int32_t bank = c == 9 ? 128 : 0, prog = 0;
        put(m, &bank, 4);
        put(m, &prog, 4);
    }
}

/* Same as build_state, plus extra (id, value) params. */
static void build_state_params(MemStream* m, const char* sfPath, const uint32_t* ids,
                               const double* vals, uint32_t n) {
    memset(m, 0, sizeof(*m));
    put_u32(m, 0x4C505346);
    put_u32(m, 1);
    put_str(m, sfPath);
    put_str(m, "");
    put_u32(m, n);
    for (uint32_t i = 0; i < n; ++i) {
        put_u32(m, ids[i]);
        put(m, &vals[i], 8);
    }
    for (int c = 0; c < 16; ++c) {
        int32_t bank = c == 9 ? 128 : 0, prog = 0;
        put(m, &bank, 4);
        put(m, &prog, 4);
    }
}

/* Peak of |sample| over n blocks. */
static float peak_blocks(const clap_plugin_t* plug, int n);

/* ------------------------------------------------------------------ main */

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <plugin.clap> <soundfont.sf2> [gui] [seconds]\n", argv[0]);
        return 2;
    }
    const char* sfPath = argv[2];
    int wantGui = argc > 3 && !strcmp(argv[3], "gui");
    int guiSeconds = argc > 4 ? atoi(argv[4]) : 4;

    lib_t dl = lib_open(argv[1]);
    if (!dl) {
        fprintf(stderr, "loading %s: %s\n", argv[1], lib_error());
        return 1;
    }
    const clap_plugin_entry_t* entry = (const clap_plugin_entry_t*)lib_sym(dl, "clap_entry");
    if (!entry || !entry->init(argv[1])) return 1;
    const clap_plugin_factory_t* factory =
        (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    printf("plugin: %s (%s) %s\n", desc->name, desc->id, desc->version);

    clap_host_t host = {.clap_version = CLAP_VERSION_INIT, .host_data = NULL,
                        .name = "mini_host", .vendor = "test", .url = "", .version = "1",
                        .get_extension = host_get_extension,
                        .request_restart = host_request_restart,
                        .request_process = host_request_process,
                        .request_callback = host_request_callback};
    const clap_plugin_t* plug = factory->create_plugin(factory, &host, desc->id);
    if (!plug || !plug->init(plug)) return 1;

    const clap_plugin_params_t* params =
        (const clap_plugin_params_t*)plug->get_extension(plug, CLAP_EXT_PARAMS);
    const clap_plugin_state_t* state =
        (const clap_plugin_state_t*)plug->get_extension(plug, CLAP_EXT_STATE);
    CHECK(params && state, "params and state extensions present");
    printf("params: %u\n", params->count(plug));

    /* State restored BEFORE activation (what most hosts do on project load). */
    MemStream ms;
    build_state(&ms, sfPath, 0);
    clap_istream_t is = {.ctx = &ms, .read = mem_read};
    CHECK(state->load(plug, &is), "state load (before activate)");

    CHECK(plug->activate(plug, SR, 32, BLOCK), "activate at %.0f Hz", SR);
    plug->start_processing(plug);

    /* 1) sample accuracy: note at frame 300, block must be silent before it */
    {
        InEvents in = {.count = 0};
        add_midi(&in, 300, 0x90, 60, 110);
        run_block(plug, &in, NULL);
        double pre = 0, post = 0;
        for (int i = 0; i < 300; ++i) pre += fabs(g_left[i]) + fabs(g_right[i]);
        for (int i = 300; i < BLOCK; ++i) post += fabs(g_left[i]) + fabs(g_right[i]);
        /* not exactly 0: fluidsynth's reverb adds a ~1e-8 anti-denormal
         * offset - anything audible would be orders of magnitude above */
        CHECK(pre < 300 * 2 * 1e-7, "silence before the note's timestamp (sum=%g)", pre);
        CHECK(post > 0.0, "sound right after the note's timestamp (sum=%g)", post);
        double rms = run_blocks(plug, 40);
        CHECK(rms > 0.001, "note sounds (peak block RMS %.4f)", rms);
        InEvents off = {.count = 0};
        add_midi(&off, 0, 0x80, 60, 0);
        run_block(plug, &off, NULL);
        run_blocks(plug, 300); /* let release + reverb die out */
    }

    /* 2) patch via host param event */
    {
        InEvents in = {.count = 0};
        add_param(&in, 0, 2 /* kParamProgram */, 40);
        add_midi(&in, 10, 0x90, 67, 100);
        run_block(plug, &in, NULL);
        double v = -1;
        params->get_value(plug, 2, &v);
        CHECK(v == 40, "Patch param set by host event (=%g)", v);
        CHECK(run_blocks(plug, 20) > 0.001, "patch 40 sounds");
        InEvents off = {.count = 0};
        add_midi(&off, 0, 0x80, 67, 0);
        run_block(plug, &off, NULL);
    }

    /* 2a) "Restore MIDI controllers" (param 36): measure CC7 with it off
     * (informative - some SoundFonts zero the standard modulators) and on
     * (must work whatever the SoundFont does). */
    for (int restore = 0; restore <= 1; ++restore) {
        InEvents in = {.count = 0};
        add_param(&in, 0, 36, restore);
        add_param(&in, 0, 2, 0);
        add_midi(&in, 0, 0xB0, 7, 100);
        run_block(plug, &in, NULL);
        run_blocks(plug, 300);
        in.count = 0;
        add_midi(&in, 0, 0x90, 60, 100);
        run_block(plug, &in, NULL);
        run_blocks(plug, 5);
        double full = run_block(plug, NULL, NULL);
        in.count = 0;
        add_midi(&in, 0, 0x80, 60, 0);
        add_midi(&in, 0, 0xB0, 7, 20);
        run_block(plug, &in, NULL);
        run_blocks(plug, 300);
        in.count = 0;
        add_midi(&in, 0, 0x90, 60, 100);
        run_block(plug, &in, NULL);
        run_blocks(plug, 5);
        double low = run_block(plug, NULL, NULL);
        in.count = 0;
        add_midi(&in, 0, 0x80, 60, 0);
        add_midi(&in, 0, 0xB0, 7, 100);
        run_block(plug, &in, NULL);
        run_blocks(plug, 300);
        if (!restore)
            printf("  info CC7=20 without restore: %.4f -> %.4f%s\n", full, low,
                   low < full * 0.25 ? "" : "  (SoundFont disables CC7)");
        else
            CHECK(low < full * 0.25, "CC7=20 with Restore MIDI controllers on (%.4f -> %.4f)",
                  full, low);
    }

    /* 2b) basic CCs act on a held note: CC7 volume, CC11 expression, CC10 pan
     * (Restore MIDI controllers stays on from 2a) */
    {
        InEvents in = {.count = 0};
        add_param(&in, 0, 2, 0);
        add_midi(&in, 0, 0xB0, 7, 100);
        add_midi(&in, 0, 0xB0, 10, 64);
        add_midi(&in, 0, 0xB0, 11, 127);
        add_midi(&in, 0, 0x90, 60, 100);
        run_blocks(plug, 300);
        run_block(plug, &in, NULL);
        run_blocks(plug, 5);
        double full = run_block(plug, NULL, NULL);
        in.count = 0;
        add_midi(&in, 0, 0xB0, 7, 20);
        run_block(plug, &in, NULL);
        run_blocks(plug, 3);
        double low = run_block(plug, NULL, NULL);
        CHECK(low < full * 0.25, "CC7 volume 100->20 lowers the level (%.4f -> %.4f)", full, low);
        in.count = 0;
        add_midi(&in, 0, 0xB0, 7, 100);
        add_midi(&in, 0, 0xB0, 11, 30);
        run_block(plug, &in, NULL);
        run_blocks(plug, 3);
        double expr = run_block(plug, NULL, NULL);
        CHECK(expr < full * 0.4, "CC11 expression lowers the level (%.4f)", expr);
        /* balance at CC10=64 first: SF2 pan is relative to each sample's
         * own pan, so compare the image before/after, not absolute L vs R */
        double l64 = 0, r64 = 0;
        in.count = 0;
        add_midi(&in, 0, 0xB0, 11, 127);
        add_param(&in, 0, 13, 0);
        add_param(&in, 0, 18, 0);
        run_block(plug, &in, NULL);
        run_blocks(plug, 3);
        run_block(plug, NULL, NULL);
        for (int i = 0; i < BLOCK; ++i) { l64 += fabs(g_left[i]); r64 += fabs(g_right[i]); }
        in.count = 0;
        add_midi(&in, 0, 0xB0, 10, 0); /* hard left */
        add_param(&in, 0, 13, 0);      /* reverb off: its tail is stereo */
        add_param(&in, 0, 18, 0);      /* chorus off */
        run_block(plug, &in, NULL);
        run_blocks(plug, 3);
        run_block(plug, NULL, NULL);
        double l = 0, r = 0;
        for (int i = 0; i < BLOCK; ++i) { l += fabs(g_left[i]); r += fabs(g_right[i]); }
        /* not a full hard-left with stereo presets: per the SF2 spec CC10
         * is added to each sample's own pan (a right-panned sample ends up
         * centered), so only check that the image moved clearly left */
        CHECK(l / (r + 1e-9) > 1.25 * l64 / (r64 + 1e-9),
              "CC10 pan moves the sound left (L/R %.2f at 64 -> %.2f at 0)",
              l64 / (r64 + 1e-9), l / (r + 1e-9));
        in.count = 0;
        add_midi(&in, 0, 0xB0, 10, 64);
        add_midi(&in, 0, 0x80, 60, 0);
        add_param(&in, 0, 13, 1);
        add_param(&in, 0, 18, 1);
        run_block(plug, &in, NULL);
    }

    /* 2c) controllers survive a synth rebuild (SoundFont reload): CC7=10
     * sent before the reload must still apply afterwards, without the host
     * resending it. */
    {
        InEvents in = {.count = 0};
        add_param(&in, 0, 2, 0);
        add_midi(&in, 0, 0xB0, 7, 100);
        run_block(plug, &in, NULL);
        in.count = 0;
        add_midi(&in, 0, 0x90, 60, 100);
        run_block(plug, &in, NULL);
        run_blocks(plug, 40);
        double full = run_block(plug, NULL, NULL);
        in.count = 0;
        add_midi(&in, 0, 0x80, 60, 0);
        add_midi(&in, 0, 0xB0, 7, 10);
        run_block(plug, &in, NULL);
        run_blocks(plug, 300);

        MemStream ms2;
        build_state(&ms2, sfPath, 0); /* same SoundFont -> fresh synth */
        clap_istream_t is3 = {.ctx = &ms2, .read = mem_read};
        state->load(plug, &is3);
        in.count = 0;
        add_param(&in, 0, 36, 1); /* the test state only stores Patch */
        add_midi(&in, 0, 0x90, 60, 100);
        run_block(plug, &in, NULL);
        run_blocks(plug, 40);
        double after = run_block(plug, NULL, NULL);
        CHECK(after < full * 0.1,
              "CC7 survives a synth rebuild (%.4f before, %.4f after reload)", full, after);
        in.count = 0;
        add_midi(&in, 0, 0x80, 60, 0);
        add_midi(&in, 0, 0xB0, 7, 100);
        add_param(&in, 0, 2, 0);
        run_block(plug, &in, NULL);
        run_blocks(plug, 300);
    }

    /* 2e) the saved patch survives a GM System On SysEx (DAWs and
     * controllers send these): fluidsynth resets every channel to program 0,
     * the plugin must put the selected patch back. Compared sample by
     * sample (effects off, from silence) against the same note before. */
    {
        static float ref[BLOCK * 2];
        InEvents in = {.count = 0};
        add_param(&in, 0, 2, 7);
        add_param(&in, 0, 13, 0);
        add_param(&in, 0, 18, 0);
        run_block(plug, &in, NULL);
        run_blocks(plug, 400);
        in.count = 0;
        add_midi(&in, 0, 0x90, 60, 100);
        run_block(plug, &in, NULL);
        memcpy(ref, g_left, sizeof(g_left));
        memcpy(ref + BLOCK, g_right, sizeof(g_right));
        in.count = 0;
        add_midi(&in, 0, 0x80, 60, 0);
        run_block(plug, &in, NULL);
        run_blocks(plug, 400);

        static const uint8_t gmOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
        in.count = 0;
        add_sysex(&in, 0, gmOn, sizeof(gmOn));
        run_block(plug, &in, NULL);
        in.count = 0;
        add_midi(&in, 0, 0x90, 60, 100);
        run_block(plug, &in, NULL);
        double diff = 0, mag = 0;
        for (int i = 0; i < BLOCK; ++i) {
            diff += fabs(g_left[i] - ref[i]) + fabs(g_right[i] - ref[BLOCK + i]);
            mag += fabs(ref[i]) + fabs(ref[BLOCK + i]);
        }
        double v = -1;
        params->get_value(plug, 2, &v);
        CHECK(v == 7, "Patch param still 7 after a GM reset SysEx (=%g)", v);
        CHECK(mag > 0 && diff < mag * 1e-3,
              "after the GM reset the note sounds exactly like patch 7 (diff %.2g of %.2g)", diff, mag);
        in.count = 0;
        add_midi(&in, 0, 0x80, 60, 0);
        add_param(&in, 0, 13, 1);
        add_param(&in, 0, 18, 1);
        add_param(&in, 0, 2, 0);
        run_block(plug, &in, NULL);
        run_blocks(plug, 300);
    }

    /* 3) MIDI Program Change in omni mode -> reported back to the host */
    {
        InEvents in = {.count = 0};
        add_midi(&in, 0, 0xC3, 25, 0); /* channel 4: omni maps to the patch */
        OutEvents out;
        memset(&out, 0, sizeof(out));
        run_block(plug, &in, &out);
        double v = -1;
        params->get_value(plug, 2, &v);
        CHECK(v == 25, "Program Change moved the Patch param (=%g)", v);
        CHECK(out.paramValueSeen[2] && out.paramValue[2] == 25,
              "plugin emitted a PARAM_VALUE event for it");
    }

    /* 4) multitimbral: channel 10 plays the drum kit */
    {
        InEvents in = {.count = 0};
        add_param(&in, 0, 28 /* kParamMidiMode */, 1);
        run_block(plug, &in, NULL);
        run_blocks(plug, 400);
        CHECK(run_blocks(plug, 5) < 0.0005, "silent before the drum check (no tails)");
        in.count = 0;
        add_midi(&in, 0, 0x99, 38, 120); /* snare on channel 10 */
        run_block(plug, &in, NULL);
        CHECK(run_blocks(plug, 10) > 0.001, "multitimbral drum note on channel 10 sounds");
        in.count = 0;
        add_midi(&in, 0, 0x95, 60, 120); /* channel 6 must be enabled too */
        run_blocks(plug, 400);
        run_block(plug, &in, NULL);
        CHECK(run_blocks(plug, 10) > 0.001, "multitimbral note on channel 6 sounds");
        in.count = 0;
        add_midi(&in, 0, 0x85, 60, 0);
        run_block(plug, &in, NULL);
        char text[64];
        params->value_to_text(plug, 28, 1, text, sizeof(text));
        printf("        (MIDI Mode text: %s)\n", text);
    }

    /* 4b) mono voice mode on channel 1 keeps working */
    {
        InEvents in = {.count = 0};
        add_param(&in, 0, 30 /* kParamVoiceMode */, 1);
        add_midi(&in, 0, 0x90, 60, 100);
        add_midi(&in, 100, 0x90, 64, 100);
        run_blocks(plug, 400);
        run_block(plug, &in, NULL);
        CHECK(run_blocks(plug, 10) > 0.001, "mono voice mode plays");
        in.count = 0;
        add_midi(&in, 0, 0x80, 60, 0);
        add_midi(&in, 0, 0x80, 64, 0);
        add_param(&in, 0, 30, 0);
        run_block(plug, &in, NULL);
    }

    /* 4c) FluidSynth 2.6 features (ids: 0 gain, 27 interpolation, 37 reverb
     * engine, 38 limiter, 39 limiter ceiling dB): a loud chord at gain 2.0
     * must stay under a -12 dB limiter ceiling; the Signalsmith reverb
     * engine and 11th-order sinc interpolation must produce sound. */
    {
        const uint32_t ids[] = {0, 2, 27, 37, 38, 39};
        const double vals[] = {2.0, 0, 3 /* sinc 11 */, 4 /* smith */, 1, -12.0};
        MemStream ms3;
        build_state_params(&ms3, sfPath, ids, vals, 6);
        clap_istream_t is5 = {.ctx = &ms3, .read = mem_read};
        CHECK(state->load(plug, &is5), "state with limiter + Signalsmith reverb + sinc 11 loads");
        InEvents in = {.count = 0};
        add_param(&in, 0, 28, 0); /* omni */
        for (int k = 0; k < 6; ++k) add_midi(&in, 0, 0x90, 48 + k * 4, 127);
        run_block(plug, &in, NULL);
        float pk = peak_blocks(plug, 40);
        CHECK(pk > 0.01f && pk <= 0.2512f * 1.02f,
              "limiter keeps the peak under -12 dBFS (peak %.3f, limit 0.251)", pk);
        in.count = 0;
        add_midi(&in, 0, 0xB0, 123, 0);
        run_block(plug, &in, NULL);
        run_blocks(plug, 300);
        /* back to plain defaults for the next tests */
        const uint32_t id0[] = {2};
        const double v0[] = {0};
        build_state_params(&ms3, sfPath, id0, v0, 1);
        clap_istream_t is6 = {.ctx = &ms3, .read = mem_read};
        state->load(plug, &is6);
        in.count = 0;
        add_param(&in, 0, 28, 1); /* multitimbral, as the following tests expect */
        run_block(plug, &in, NULL);
    }

    /* 4d) clap.render: offline rendering switches to Render Interpolation
     * (param 42, default 6 = 25th-order sinc); 0 = same as playback. The
     * same note (from silence, effects off, transposed off the sample root
     * so interpolation matters) is compared sample by sample. */
    {
        const clap_plugin_render_t* render =
            (const clap_plugin_render_t*)plug->get_extension(plug, CLAP_EXT_RENDER);
        CHECK(render != NULL, "clap.render extension present");
        static float refRt[BLOCK * 4];
        float cur[BLOCK * 4];
        InEvents in = {.count = 0};
        add_param(&in, 0, 13, 0);
        add_param(&in, 0, 18, 0);
        add_param(&in, 0, 2, 0);
        run_block(plug, &in, NULL);
        run_blocks(plug, 400);
        for (int pass = 0; pass < 3; ++pass) {
            /* pass 0: realtime; 1: offline (sinc 25); 2: offline, "same as playback" */
            if (render) render->set(plug, pass == 0 ? CLAP_RENDER_REALTIME : CLAP_RENDER_OFFLINE);
            in.count = 0;
            add_param(&in, 0, 42, pass == 2 ? 0 : 6);
            add_midi(&in, 0, 0x90, 61, 100);
            run_block(plug, &in, NULL);
            memcpy(cur, g_left, sizeof(g_left));
            run_block(plug, NULL, NULL);
            memcpy(cur + BLOCK, g_left, sizeof(g_left));
            double diff = 0, mag = 0;
            for (int i = 0; i < BLOCK * 2; ++i) {
                if (pass == 0) refRt[i] = cur[i];
                diff += fabs(cur[i] - refRt[i]);
                mag += fabs(refRt[i]);
            }
            if (pass == 1)
                CHECK(mag > 0 && diff > mag * 1e-4,
                      "offline render uses another interpolation (diff %.3g of %.3g)", diff, mag);
            if (pass == 2)
                /* not exactly 0 with long-release presets (tails of the
                 * previous pass); a real method change is ~100x bigger */
                CHECK(mag > 0 && diff < mag * 5e-5,
                      "'Same as playback' renders identically (diff %.3g of %.3g)", diff, mag);
            in.count = 0;
            add_midi(&in, 0, 0x80, 61, 0);
            run_block(plug, &in, NULL);
            run_blocks(plug, 400);
        }
        if (render) render->set(plug, CLAP_RENDER_REALTIME);
        in.count = 0;
        add_param(&in, 0, 42, 6);
        add_param(&in, 0, 13, 1);
        add_param(&in, 0, 18, 1);
        run_block(plug, &in, NULL);
    }

    /* 5) state round trip */
    {
        MemStream out;
        memset(&out, 0, sizeof(out));
        clap_ostream_t os = {.ctx = &out, .write = mem_write};
        CHECK(state->save(plug, &os), "state save (%llu bytes)", (unsigned long long)out.len);
        int pathFound = contains_bytes(out.buf, out.len, sfPath, strlen(sfPath));
        CHECK(pathFound, "saved state references the SoundFont path");
        clap_istream_t is2 = {.ctx = &out, .read = mem_read};
        CHECK(state->load(plug, &is2), "state load (while active)");
        double v = -1;
        params->get_value(plug, 28, &v);
        CHECK(v == 1, "MIDI Mode survived the round trip (=%g)", v);
        InEvents in = {.count = 0};
        add_param(&in, 0, 2, 0); /* patch 0 exists in every GM-ish SoundFont */
        add_midi(&in, 0, 0x90, 64, 100);
        run_block(plug, &in, NULL);
        CHECK(run_blocks(plug, 10) > 0.001, "plays after reloading state while active");
    }

    if (wantGui) {
        const clap_plugin_gui_t* gui =
            (const clap_plugin_gui_t*)plug->get_extension(plug, CLAP_EXT_GUI);
        if (gui && gui->is_api_supported(plug, GUI_API, false) &&
            gui->create(plug, GUI_API, false)) {
            uint32_t w = 0, h = 0;
            gui->get_size(plug, &w, &h);
#ifdef _WIN32
            HWND parent = create_parent_window(w, h);
            clap_window_t win = {.api = GUI_API, .win32 = parent};
            if (!gui->set_parent(plug, &win)) {
                printf("  FAIL set_parent\n");
                ++g_failures;
            }
#endif
            gui->show(plug);
            printf("gui shown (%ux%u) for %d s\n", w, h, guiSeconds);
            /* keep "playing" so meters/piano/voices are alive on screen */
            InEvents in = {.count = 0};
            add_midi(&in, 0, 0x90, 48, 90);
            add_midi(&in, 0, 0x90, 55, 90);
            add_midi(&in, 0, 0x90, 64, 90);
            run_block(plug, &in, NULL);
            for (int i = 0; i < guiSeconds * 94; ++i) {
                run_block(plug, NULL, NULL);
                wait_ms_pumping(10);
            }
            gui->destroy(plug);
#ifdef _WIN32
            DestroyWindow(parent);
#endif
            printf("gui destroyed\n");
        } else {
            printf("gui not available\n");
            ++g_failures;
        }
    }

    plug->stop_processing(plug);
    plug->deactivate(plug);
    plug->destroy(plug);

    /* 6) default SoundFont (global config "defaultsf="): applied to a
     * brand-new instance, never to one restoring a saved project. */
    {
        char cfgDir[512];
        if (!make_temp_dir(cfgDir, sizeof(cfgDir))) { perror("temp dir"); return 1; }
        char path[600];
        snprintf(path, sizeof(path), "%s/FluidSynthPlayer", cfgDir);
        make_dir(path);
        snprintf(path, sizeof(path), "%s/FluidSynthPlayer/config.txt", cfgDir);
        FILE* f = fopen(path, "w");
        fprintf(f, "editor=polyphone\ndefaultsf=%s\n", sfPath);
        fclose(f);
        set_env("XDG_CONFIG_HOME", cfgDir);

        for (int restore = 0; restore <= 1; ++restore) {
            const clap_plugin_t* p2 = factory->create_plugin(factory, &host, desc->id);
            p2->init(p2);
            const clap_plugin_state_t* st2 =
                (const clap_plugin_state_t*)p2->get_extension(p2, CLAP_EXT_STATE);
            if (restore) { /* a saved project with no SoundFont */
                MemStream empty;
                build_state(&empty, "", 0);
                clap_istream_t is4 = {.ctx = &empty, .read = mem_read};
                st2->load(p2, &is4);
            }
            p2->activate(p2, SR, 32, BLOCK);
            p2->start_processing(p2);
            InEvents in = {.count = 0};
            add_midi(&in, 0, 0x90, 60, 100);
            run_block(p2, &in, NULL);
            double rms = run_blocks(p2, 20);
            MemStream out;
            memset(&out, 0, sizeof(out));
            clap_ostream_t os = {.ctx = &out, .write = mem_write};
            st2->save(p2, &os);
            int hasPath = contains_bytes(out.buf, out.len, sfPath, strlen(sfPath));
            if (!restore) {
                CHECK(rms > 0.001 && hasPath,
                      "new instance starts with the default SoundFont (RMS %.4f, in state: %d)",
                      rms, hasPath);
            } else {
                CHECK(rms < 1e-6 && !hasPath,
                      "restored project without SoundFont ignores the default (RMS %.6f)", rms);
            }
            p2->stop_processing(p2);
            p2->deactivate(p2);
            p2->destroy(p2);
        }
        remove(path);
        snprintf(path, sizeof(path), "%s/FluidSynthPlayer", cfgDir);
        remove_dir(path);
        remove_dir(cfgDir);
    }

    entry->deinit();
    lib_close(dl);
    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL OK\n", g_failures);
    return g_failures ? 1 : 0;
}
