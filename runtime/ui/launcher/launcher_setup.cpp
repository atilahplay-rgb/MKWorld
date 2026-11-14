// The game files setup of the start screen (launcher_setup.h): prepare.py in a child process (SDL_Process), its event
// lines read by a thread of ours into the state the page shows. The steps and their order are prepare.py's; this side
// only maps their names. totk-pc-recomp's installer window works the same way over its CLI (installer/gui.py, MIT).
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/launcher/launcher_setup.h"

extern "C" {
#include "platform/platform.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char rt_port_version[];
}

static const char *const kSteps[SETUP_STEPS] = {"check", "exefs", "romfs", "generate", "compile", "shaders", "warm",
                                               "cleanup"};

static SDL_Mutex *g_lock;
static SetupState g_state = {0, -1, {}, 0, 0, 0, {}, {}};
static SDL_Process *g_proc;
static char g_times_path[1200];     // where the last setup's step times are kept (next to the game files)
static long long g_rate_t;          // the last progress line's time and count: the rate is measured between them
static long g_rate_p;

// What each step took in the last setup that finished: one line, eight numbers in ms (-1: that setup kept the step).
// Missing or damaged: nothing is known and the page counts only the step under way.
static void load_times(long long *out) {
    for (int i = 0; i < SETUP_STEPS; i++) out[i] = 0;
    if (!g_times_path[0]) return;
    size_t n = 0;
    char *data = (char *)SDL_LoadFile(g_times_path, &n);   // zero-terminated
    if (!data) return;
    long long v[SETUP_STEPS];
    const char *at = data;
    bool ok = true;
    for (int i = 0; i < SETUP_STEPS && ok; i++) {
        char *next = nullptr;
        v[i] = strtoll(at, &next, 10);
        ok = next != at && v[i] >= -1 && v[i] < 24LL * 3600 * 1000;
        at = next;
    }
    if (ok)
        for (int i = 0; i < SETUP_STEPS; i++) out[i] = v[i];
    SDL_free(data);
}

static void save_times(const SetupState &s) {
    if (!g_times_path[0]) return;
    char buf[256];
    size_t n = 0;
    for (int i = 0; i < SETUP_STEPS && n < sizeof buf; i++) {
        long long v = s.done[i] == 2 ? -1 : s.step_ms[i];
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%lld%c", v, i + 1 < SETUP_STEPS ? ' ' : '\n');
    }
    if (n < sizeof buf && !SDL_SaveFile(g_times_path, buf, n)) rt_log("setup: cannot save %s: %s", g_times_path, SDL_GetError());
}

// The Python and prepare.py: the package's (setup/python, setup/src) beside the executable, else a source checkout's
// (the executable in build/: the repository's .venv and tools/). rel names the tool under tools/ ("prepare/prepare.py").
static bool find_tool(const char *exe_dir, const char *rel, char *python, char *script, size_t cap) {
#ifdef _WIN32
    snprintf(python, cap, "%s/setup/python/python.exe", exe_dir);
#else
    snprintf(python, cap, "%s/setup/python/bin/python3", exe_dir);
#endif
    snprintf(script, cap, "%s/setup/src/tools/%s", exe_dir, rel);
    if (plat_file_exists(python) && plat_file_exists(script)) return true;
#ifdef _WIN32
    snprintf(python, cap, "%s/../.venv/Scripts/python.exe", exe_dir);
#else
    snprintf(python, cap, "%s/../.venv/bin/python3", exe_dir);
#endif
    snprintf(script, cap, "%s/../tools/%s", exe_dir, rel);
    return plat_file_exists(python) && plat_file_exists(script);
}

int setup_available(const char *exe_dir) {
    char python[1200], script[1200];
    return find_tool(exe_dir, "prepare/prepare.py", python, script, sizeof python);
}

// The package's Python and a tool under tools/ (import.py), for pages that run scripts like the setup does.
int setup_tool(const char *exe_dir, const char *rel, char *python, char *script, size_t cap) {
    return find_tool(exe_dir, rel, python, script, cap);
}

static void set_finished(int how, const char *code, const char *why) {
    SDL_LockMutex(g_lock);
    g_state.running = 0;
    g_state.finished = how;
    if (g_state.timed && !g_state.t_end) g_state.t_end = (long long)SDL_GetTicks();
    if (code) snprintf(g_state.code, sizeof g_state.code, "%s", code);
    if (why) snprintf(g_state.error, sizeof g_state.error, "%s", why);
    SDL_UnlockMutex(g_lock);
}

// One event line of prepare.py (its header lists them).
static void on_line(char *line) {
    rt_log("setup: %s", line);
    long long now = (long long)SDL_GetTicks();
    SDL_LockMutex(g_lock);
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "step") || !strcmp(line, "skip")) {
        for (int i = 0; arg && i < SETUP_STEPS; i++)
            if (!strcmp(arg, kSteps[i])) {
                if (g_state.step >= 0 && g_state.step != i && !g_state.done[g_state.step]) {
                    g_state.done[g_state.step] = 1;
                    g_state.step_ms[g_state.step] = now - g_state.step_t0;
                }
                if (line[1] == 'k') g_state.done[i] = 2;
                else {
                    g_state.step = i, g_state.progress = g_state.total = 0;
                    g_state.step_t0 = now, g_state.rate = 0;
                    g_rate_t = now, g_rate_p = 0;
                }
            }
    } else if (!strcmp(line, "progress") && arg) {
        long p = 0, t = 0;
        if (sscanf(arg, "%ld %ld", &p, &t) == 2) {
            // The speed of the step is measured between progress lines and smoothed over ~30 s (a time constant, so
            // lines that come close together count as much as the time they cover): the compile starts with its
            // largest files, so what it did at the start says little about the end.
            long long dt = now - g_rate_t;
            if (p < g_rate_p) {   // counted again from the start (a retry): measure anew
                g_state.rate = 0;
                g_rate_t = now, g_rate_p = p;
            } else if (dt > 0) {
                double inst = (double)(p - g_rate_p) / (double)dt;
                if (g_state.rate <= 0) g_state.rate = p > g_rate_p ? inst : 0;
                else g_state.rate += (inst - g_state.rate) * (1.0 - exp(-(double)dt / 30000.0));
                g_rate_t = now, g_rate_p = p;
            }
            g_state.progress = p, g_state.total = t;
        }
    } else if (!strcmp(line, "done")) {
        if (g_state.step >= 0 && !g_state.done[g_state.step]) g_state.step_ms[g_state.step] = now - g_state.step_t0;
        for (int i = 0; i < SETUP_STEPS; i++)
            if (!g_state.done[i]) g_state.done[i] = 1;
    } else if (!strcmp(line, "error") && arg) {
        char *why = strchr(arg, ' ');
        if (why) *why++ = 0;
        snprintf(g_state.code, sizeof g_state.code, "%s", arg);
        snprintf(g_state.error, sizeof g_state.error, "%s", why ? why : "");
    }
    SDL_UnlockMutex(g_lock);
}

static int SDLCALL reader(void *) {
    SDL_IOStream *out = SDL_GetProcessOutput(g_proc);
    char line[2048];
    size_t n = 0;
    char c;
    for (;;) {
        if (SDL_ReadIO(out, &c, 1) != 1) {   // the pipe does not block: no data yet is not the end
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
            SDL_Delay(20);
            continue;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n + 1 < sizeof line) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        on_line(line);
    }
    int code = -1;
    SDL_WaitProcess(g_proc, true, &code);
    SDL_DestroyProcess(g_proc);
    g_proc = nullptr;
    SetupState s = setup_state();
    bool told = code == 0 || s.code[0];
    set_finished(code == 0 ? 1 : -1, told ? nullptr : "stopped", told ? nullptr : "");
    if (code == 0) save_times(setup_state());   // the next setup counts its steps from these
    rt_log("setup: ended with code %d", code);
    return 0;
}

void setup_start(const char *exe_dir, const char *source, int link_romfs) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (setup_state().running) return;
    char python[1200], script[1200];
    snprintf(g_times_path, sizeof g_times_path, "%s/setup-times.txt", exe_dir);
    long long prev[SETUP_STEPS];
    load_times(prev);
    SDL_LockMutex(g_lock);
    g_state = SetupState{1, -1, {}, 0, 0, 0, {}, {}};
    g_state.timed = 1;
    g_state.t_start = (long long)SDL_GetTicks();
    for (int i = 0; i < SETUP_STEPS; i++) g_state.prev_ms[i] = prev[i];
    SDL_UnlockMutex(g_lock);
    if (!find_tool(exe_dir, "prepare/prepare.py", python, script, sizeof python)) {
        set_finished(-1, "no_setup", "");
        return;
    }
    // -I: no user site packages or PYTHON* variables, nothing from the working directory
    // without a source, prepare.py takes the last setup's (and its RomFS mode) and the arguments end at the version
    const char *args[] = {python, "-I", script, "--game", exe_dir, "--port", rt_port_version, "--source", source,
                          "--romfs", link_romfs ? "link" : "copy", nullptr};
    if (!source) args[7] = nullptr;
    rt_log("setup: %s %s --game %s --source %s --romfs %s", python, script, exe_dir, source ? source : "(last)",
           link_romfs ? "link" : "copy");
    g_proc = SDL_CreateProcess(args, true);
    if (!g_proc) {
        set_finished(-1, "", SDL_GetError());
        return;
    }
    SDL_DetachThread(SDL_CreateThread(reader, "setup", nullptr));
}

void setup_cancel(void) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (g_proc) SDL_KillProcess(g_proc, true);
    snprintf(g_state.code, sizeof g_state.code, "cancelled");
    g_state.error[0] = 0;
    SDL_UnlockMutex(g_lock);
}

SetupState setup_state(void) {
    if (!g_lock) return g_state;
    SDL_LockMutex(g_lock);
    SetupState s = g_state;
    SDL_UnlockMutex(g_lock);
    return s;
}

long long setup_elapsed_ms(const SetupState &st) {
    if (!st.timed) return 0;
    long long end = st.running ? (long long)SDL_GetTicks() : st.t_end;
    return end > st.t_start ? end - st.t_start : 0;
}

long long setup_eta_ms(const SetupState &st, int *partial) {
    if (partial) *partial = 0;
    if (!st.running || st.step < 0) return -1;
    long long now = (long long)SDL_GetTicks(), left = 0;
    if (!st.done[st.step]) {
        bool settled = now - st.step_t0 >= 4000;   // the first seconds of a step say little about its speed
        if (settled && st.total > 0 && st.progress > 0 && st.rate > 0)
            left = (long long)((double)(st.total - st.progress) / st.rate);
        else if (st.prev_ms[st.step] > 0)
            left = st.prev_ms[st.step] - (now - st.step_t0);
        else
            return -1;
        if (left < 0) left = 0;
    }
    int part = 0;
    for (int j = st.step + 1; j < SETUP_STEPS; j++) {
        if (st.done[j]) continue;
        if (st.prev_ms[j] > 0) left += st.prev_ms[j];
        else if (st.prev_ms[j] == 0) part = 1;   // -1: the last setup kept it, nothing to add
    }
    if (partial) *partial = part;
    return left;
}
