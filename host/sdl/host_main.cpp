// pico-amiga on a PC: the firmware (src/pico/main.c and everything it calls) in an
// SDL2 window, after pico-speccy's host build (its pico-speccy-host branch).
//
// Threads, mirroring the RP2350:
//   core0  - the firmware's main() (built as firmware_main), the emulator loop;
//   core1  - what main.c launches there (video_start, then idle);
//   vsync  - the output frame clock video_frames() counts (host_drivers.cpp);
//   main   - SDL: window, events, presenting the framebuffer; the audio callback.
// The "SD card" is a folder, ~/pico-amiga by default (--root DIR picks another):
// host_fatfs.cpp is the FatFs API over it, so the firmware reads /amiga/kick.rom
// and /amiga/df0.adf from <root>/amiga/ exactly as from a card.
// Keyboard: SDL scancodes are USB HID usages, so keys become HID boot reports for
// the firmware's own process_kbd_report() (input.cpp) on core0.  Mouse: click into
// the window to capture it (relative mode), middle button releases it.
// Headless checks: PICO_AMIGA_SHOT=file.bmp:N[,file2.bmp:M...] saves the picture at
// output frame N; PICO_AMIGA_KEYS=N:Key[,M:Key...] presses a key (an SDL scancode
// name: F1, Down, Return, Escape..., or MouseL / MouseR) at output frame N for 4
// frames.
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include "pico_host.h"
#include "host_sdl.h"
extern "C" {
#include "amiga.h"
#include "psram.h"
#include "video.h"
}

extern "C" int firmware_main(void);
extern std::string host_root;           // host_fatfs.cpp
char **host_argv;                       // host_platform.cpp: a firmware reboot re-execs
extern "C" void host_sd_flush(void) {}  // host_platform.cpp: no raw card in this build

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--root DIR]\n"
                    "  DIR is the SD card folder (default ~/pico-amiga): DIR/amiga/kick.rom,\n"
                    "  DIR/amiga/ext.rom, DIR/amiga/df0.adf as on the card.\n", argv0);
}

int main(int argc, char **argv) {
    host_argv = argv;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) host_root = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    // The firmware takes its memory from the PSRAM window at PSRAM_BASE.
    void *ps = mmap((void *)(uintptr_t)PSRAM_BASE, HOST_PSRAM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ps != (void *)(uintptr_t)PSRAM_BASE) {
        fprintf(stderr, "host: cannot map the PSRAM window at 0x%08X\n", PSRAM_BASE);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "host: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("pico-amiga - click to capture the mouse, middle button releases it",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       AMIGA_FB_W * 2, AMIGA_FB_H * 2, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) {
        fprintf(stderr, "host: SDL window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, AMIGA_FB_W * 2, AMIGA_FB_H * 2);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         AMIGA_FB_W, AMIGA_FB_H);

    std::thread([] { firmware_main(); }).detach();

    struct Shot { std::string file; long frame; };
    struct Key { long frame; SDL_Scancode sc; int state; };   // 0 to press, 1 down, 2 done
    std::vector<Shot> shots;
    std::vector<Key> keys;
    auto split = [](const char *list, auto fn) {
        if (!list) return;
        std::string l(list);
        size_t p = 0;
        while (p <= l.size()) {
            size_t e = l.find(',', p);
            if (e == std::string::npos) e = l.size();
            const std::string item = l.substr(p, e - p);
            const size_t c = item.rfind(':');
            if (c != std::string::npos) fn(item.substr(0, c), item.substr(c + 1));
            p = e + 1;
        }
    };
    split(getenv("PICO_AMIGA_SHOT"), [&](const std::string &a, const std::string &b) {
        shots.push_back({a, atol(b.c_str())});
    });
    // The key list is "frame:Key": the frame comes first.
    split(getenv("PICO_AMIGA_KEYS"), [&](const std::string &a, const std::string &b) {
        const size_t c = a.find(':');
        (void)c;
        // MouseL / MouseR: the mouse buttons (games that wait for a click).
        const SDL_Scancode sc = b == "MouseL" ? (SDL_Scancode)(SDL_NUM_SCANCODES + 1)
                              : b == "MouseR" ? (SDL_Scancode)(SDL_NUM_SCANCODES + 2)
                              : SDL_GetScancodeFromName(b.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) fprintf(stderr, "host: unknown key '%s'\n", b.c_str());
        else keys.push_back({atol(a.c_str()), sc, 0});
    });

    bool captured = false;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                SDL_Quit();
                _exit(0);
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (!e.key.repeat) host_key(e.key.keysym.scancode, e.type == SDL_KEYDOWN);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_MIDDLE) {
                    captured = false;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                } else if (!captured) {
                    captured = true;              // the capturing click is not passed on
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                    break;
                }
                /* fall through */
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEMOTION:
                if (captured) {
                    const Uint32 b = SDL_GetMouseState(NULL, NULL);
                    const int dx = e.type == SDL_MOUSEMOTION ? e.motion.xrel : 0;
                    const int dy = e.type == SDL_MOUSEMOTION ? e.motion.yrel : 0;
                    host_mouse(dx, dy, b & SDL_BUTTON_LMASK, b & SDL_BUTTON_RMASK);
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && captured) {
                    captured = false;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
                break;
            default:
                break;
            }
        }

        const long vf = (long)video_frames();
        for (Key &k : keys) {
            const bool down = k.state == 0 && vf >= k.frame, up = k.state == 1 && vf >= k.frame + 4;
            if (!down && !up) continue;
            if (k.sc > SDL_NUM_SCANCODES)
                host_mouse(0, 0, down && k.sc == SDL_NUM_SCANCODES + 1, down && k.sc == SDL_NUM_SCANCODES + 2);
            else
                host_key(k.sc, down);
            k.state = down ? 1 : 2;
        }

        const uint16_t *fb = host_fb;
        if (fb) {
            void *pixels;
            int pitch;
            if (SDL_LockTexture(tex, NULL, &pixels, &pitch) == 0) {
                for (int y = 0; y < AMIGA_FB_H; y++) {
                    uint32_t *d = (uint32_t *)((uint8_t *)pixels + y * pitch);
                    const uint16_t *s = fb + y * AMIGA_FB_W;
                    for (int x = 0; x < AMIGA_FB_W; x++) {
                        const uint32_t c = s[x];
                        d[x] = 0xFF000000u | ((c >> 8) & 15) * 0x110000u | ((c >> 4) & 15) * 0x1100u |
                               (c & 15) * 0x11u;
                    }
                }
                SDL_UnlockTexture(tex);
            }
            for (Shot &sh : shots) {
                if (sh.frame < 0 || vf < sh.frame) continue;
                SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, AMIGA_FB_W, AMIGA_FB_H, 32, SDL_PIXELFORMAT_ARGB8888);
                for (int i = 0; i < AMIGA_FB_W * AMIGA_FB_H; i++) {
                    const uint32_t c = fb[i];
                    ((uint32_t *)sf->pixels)[(i / AMIGA_FB_W) * (sf->pitch / 4) + i % AMIGA_FB_W] =
                        0xFF000000u | ((c >> 8) & 15) * 0x110000u | ((c >> 4) & 15) * 0x1100u | (c & 15) * 0x11u;
                }
                SDL_SaveBMP(sf, sh.file.c_str());
                SDL_FreeSurface(sf);
                fprintf(stderr, "host: frame %ld saved to %s\n", sh.frame, sh.file.c_str());
                sh.frame = -1;
            }
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);   // V-Sync paces this loop
    }
}
