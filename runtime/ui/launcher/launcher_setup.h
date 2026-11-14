// The game files setup the start screen runs (ui/launcher/launcher_setup.cpp): tools/prepare/prepare.py in a child process, with
// the package's Python (setup/ next to the executable) or, in a source checkout, the repository's.
#pragma once

enum { SETUP_STEPS = 8 };   // prepare.py STEPS: check, exefs, romfs, generate, compile, shaders, warm, cleanup

typedef struct {
    int running;
    int step;                // the step being run, 0..SETUP_STEPS-1; -1 none yet
    int done[SETUP_STEPS];   // 1 run, 2 skipped (already done by an earlier setup)
    long progress, total;    // of the step being run; total 0: no count
    int finished;            // 1 done, -1 failed or cancelled
    char code[16];           // why, when it failed: prepare.py's error code (its header), or "stopped", "cancelled",
                             // "no_setup" (the tools missing) and "" for a message of SDL's
    char error[512];         // the detail: a folder, a build id, a tool's English message...
    // Times, in ms of SDL_GetTicks (launcher_setup.cpp), for the page's elapsed time and the time left:
    int timed;                      // 1 once a setup has been started by this run of the program
    long long t_start, t_end;       // t_end 0 while it runs
    long long step_t0;              // when the step under way began
    long long step_ms[SETUP_STEPS]; // how long each step that ran took
    long long prev_ms[SETUP_STEPS]; // the same in the last setup that finished (setup-times.txt): -1 kept, 0 not known
    double rate;                    // progress units per ms of the step under way, smoothed; 0 not known yet
} SetupState;

int setup_available(const char *exe_dir);   // the Python and the script are there
// The package's Python and tools/<rel> (import.py), for pages that run scripts like the setup does.
int setup_tool(const char *exe_dir, const char *rel, char *python, char *script, size_t cap);
// source NULL: the last setup's copy and RomFS mode (after an update: only what changed runs again)
void setup_start(const char *exe_dir, const char *source, int link_romfs);
void setup_cancel(void);
SetupState setup_state(void);                // a copy, any time
// ms since the setup started (frozen when it ends); 0 if none was started
long long setup_elapsed_ms(const SetupState &st);
// ms the running setup still needs, -1 while it cannot tell. The step under way is counted from its own rate (the
// count of its progress lines) or, before that, from its time in the last setup; the later steps from the last setup.
// *partial is set when some later step has no time yet (a first setup): the number is then a lower bound.
long long setup_eta_ms(const SetupState &st, int *partial);
