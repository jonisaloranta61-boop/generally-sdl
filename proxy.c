/* GeneRally SDL.dll proxy - koe 1: vain lokitus.
   Alkuperäinen SDL.dll nimetään SDL_real.dll:ksi, tämä tiedosto on uusi SDL.dll.
   Loki kirjoitetaan tiedostoon gr_log.txt pelin kansioon. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int Uint32;
typedef unsigned char Uint8;
#define EXP __declspec(dllexport)

static HINSTANCE hSelf = 0;
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) hSelf = h;
    return TRUE;
}

static HMODULE R = 0;
static FILE *LG = 0;
static Uint32 (*r_GetTicks)(void);
static void (*r_Delay)(Uint32);
static Uint8 *(*r_GetKeyState)(int *);
static int (*r_PollEvent)(void *);
static void (*r_PumpEvents)(void);
static int (*r_Flip)(void *);

static unsigned cGetTicks, cDelay, cGetKey, cPoll, cPump, cFlip;
static DWORD lastRep;
static Uint32 lastFlip;
static Uint32 dtMin = 0xffffffffu, dtMax = 0;
static double dtSum;
static unsigned dtN;
static unsigned delayLogged;
static Uint8 prevKeys[512];

static void init(void) {
    if (R) return;
    char path[MAX_PATH];
    DWORD err1 = 0, err2 = 0;
    path[0] = 0;
    LG = fopen("gr_log.txt", "w");
    R = GetModuleHandleA("SDL_real.dll");
    if (!R && GetModuleFileNameA(hSelf, path, MAX_PATH)) {
        char *s = strrchr(path, '\\');
        if (!s) s = strrchr(path, '/');
        if (s) strcpy(s + 1, "SDL_real.dll");
        R = LoadLibraryA(path);
        if (!R) err1 = GetLastError();
    }
    if (!R) {
        R = LoadLibraryA("SDL_real.dll");
        if (!R) err2 = GetLastError();
    }
    if (LG) {
        fprintf(LG, "tried path: %s | error1=%lu error2=%lu\n", path, (unsigned long)err1,
                (unsigned long)err2);
    }
    if (R) {
        r_GetTicks = (Uint32(*)(void))GetProcAddress(R, "SDL_GetTicks");
        r_Delay = (void (*)(Uint32))GetProcAddress(R, "SDL_Delay");
        r_GetKeyState = (Uint8 * (*)(int *)) GetProcAddress(R, "SDL_GetKeyState");
        r_PollEvent = (int (*)(void *))GetProcAddress(R, "SDL_PollEvent");
        r_PumpEvents = (void (*)(void))GetProcAddress(R, "SDL_PumpEvents");
        r_Flip = (int (*)(void *))GetProcAddress(R, "SDL_Flip");
    }
    if (LG) {
        fprintf(LG, "proxy start, real SDL loaded: %s\n", R ? "yes" : "NO");
        fflush(LG);
    }
    lastRep = GetTickCount();
}

static void report(void) {
    DWORD n = GetTickCount();
    if (n - lastRep < 1000 || !LG) return;
    fprintf(LG,
            "[sec] GetTicks=%u Delay=%u GetKeyState=%u PollEvent=%u PumpEvents=%u Flip=%u"
            " | flip-dt ms min/avg/max = %u/%.1f/%u\n",
            cGetTicks, cDelay, cGetKey, cPoll, cPump, cFlip,
            dtN ? dtMin : 0, dtN ? dtSum / dtN : 0.0, dtMax);
    fflush(LG);
    cGetTicks = cDelay = cGetKey = cPoll = cPump = cFlip = 0;
    dtMin = 0xffffffffu;
    dtMax = 0;
    dtSum = 0;
    dtN = 0;
    lastRep = n;
}

EXP Uint32 SDL_GetTicks(void) {
    init();
    cGetTicks++;
    Uint32 t = r_GetTicks();
    report();
    return t;
}

EXP void SDL_Delay(Uint32 ms) {
    init();
    cDelay++;
    if (LG && delayLogged < 40) {
        fprintf(LG, "Delay(%u) at ticks %u\n", ms, r_GetTicks());
        delayLogged++;
    }
    r_Delay(ms);
}

EXP Uint8 *SDL_GetKeyState(int *numkeys) {
    init();
    cGetKey++;
    int n0 = 0;
    Uint8 *k = r_GetKeyState(&n0);
    if (numkeys) *numkeys = n0;
    if (k && LG) {
        int n = (n0 > 0 && n0 <= 512) ? n0 : 323;
        for (int i = 0; i < n; i++) {
            if (k[i] != prevKeys[i]) {
                fprintf(LG, "key %d -> %d at ticks %u\n", i, k[i], r_GetTicks());
                prevKeys[i] = k[i];
            }
        }
    }
    return k;
}

EXP int SDL_PollEvent(void *e) {
    init();
    cPoll++;
    return r_PollEvent(e);
}

EXP void SDL_PumpEvents(void) {
    init();
    cPump++;
    r_PumpEvents();
}

EXP int SDL_Flip(void *s) {
    init();
    cFlip++;
    Uint32 t = r_GetTicks();
    if (lastFlip) {
        Uint32 d = t - lastFlip;
        if (d < dtMin) dtMin = d;
        if (d > dtMax) dtMax = d;
        dtSum += d;
        dtN++;
    }
    lastFlip = t;
    return r_Flip(s);
}
