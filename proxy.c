/* GeneRally SDL.dll proxy - koe 2: virtuaalikello, nauhoitus/toisto, kuvan tarkistussumma.
   Alkuperäinen SDL.dll nimetään SDL_real.dll:ksi, tämä tiedosto on uusi SDL.dll.
   Asetukset: tiedosto gr_net.cfg pelin kansiossa (rivit: mode=off|record|replay, seed=12345, clock=real).
   Lokit: gr_log.txt, nauhoitus: gr_keys.txt. */
#define WIN32_LEAN_AND_MEAN
#include "net.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
static int (*r_OpenAudio)(void *, void *);
static char NET_ROOM[48] = "kokeilu1";
static char NET_BROKER[96] = "broker.hivemq.com";
static int NET_PORT = 1883, NET_DELAY = 8, NET_WAIT = 1800; /* odotus sekunteina */
static Uint8 netKeys[512];
static unsigned char evq[512][20];
static unsigned evh, evt;
static unsigned netTick, nextTickV;
static NIn carry;
static int remapFrom[32] = {119, 115, 97, 100}, remapTo[32] = {273, 274, 276, 275}; /* vieraan w,s,a,d -> nuolet */
static int nRemap = 4;
static int AUDIO_OFF = 1; /* v4: aani pois oletuksena (audio=on gr_net.cfg:ssa palauttaa) */
static FILE *TR;
static unsigned totRand;
#define TRACE(c) do { if (TR && S <= 16000) fprintf(TR, "%u %c %u %u\n", S, c, V, totRand); } while (0)

/* ---- asetukset ---- */
static int MODE = 3;   /* 0 pois, 1 nauhoita, 2 toista, 3 auto (oletus) */
static int VCLOCK = 1; /* 1 = virtuaalikello */
static unsigned SEED = 12345;

/* ---- laskurit ---- */
static unsigned cGetTicks, cDelay, cGetKey, cPoll, cPump, cFlip;
static unsigned cSrand, cRand, cTime, cQpc, cGst, cGstft;
static unsigned pollIdx, frameNo;
static unsigned S; /* globaali kutsulaskuri: nauhoitus ja toisto sidotaan siihen */
static Uint32 V = 1000; /* virtuaaliaika ms */
static unsigned stuck;
static Uint8 prevKeys[512];
static Uint8 repKeys[512];
static DWORD lastRep;
static unsigned delayLogged;

/* ---- nauhoitus ---- */
typedef struct { unsigned idx; int key, val; } KRec;
typedef struct { unsigned idx; Uint8 b[20]; } ERec;
static KRec *krec; static unsigned nk, ak, rk;
static ERec *erec; static unsigned ne, ae, re;
static FILE *KF;

static void add_k(unsigned idx, int key, int val) {
    if (nk == ak) { ak = ak ? ak * 2 : 4096; krec = (KRec *)realloc(krec, ak * sizeof(KRec)); }
    krec[nk].idx = idx; krec[nk].key = key; krec[nk].val = val; nk++;
}
static void add_e(unsigned idx, const Uint8 *b) {
    if (ne == ae) { ae = ae ? ae * 2 : 4096; erec = (ERec *)realloc(erec, ae * sizeof(ERec)); }
    erec[ne].idx = idx; memcpy(erec[ne].b, b, 20); ne++;
}

static void load_cfg(void) {
    FILE *f = fopen("gr_net.cfg", "r");
    char line[128];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "mode=off", 8)) MODE = 0;
        else if (!strncmp(line, "mode=record", 11)) MODE = 1;
        else if (!strncmp(line, "mode=replay", 11)) MODE = 2;
        else if (!strncmp(line, "clock=real", 10)) VCLOCK = 0;
        else if (!strncmp(line, "audio=on", 8)) AUDIO_OFF = 0;
        else if (!strncmp(line, "mode=net", 8)) MODE = 4;
        else if (!strncmp(line, "room=", 5)) { sscanf(line + 5, "%47s", NET_ROOM); }
        else if (!strncmp(line, "broker=", 7)) { sscanf(line + 7, "%95s", NET_BROKER); }
        else if (!strncmp(line, "port=", 5)) NET_PORT = atoi(line + 5);
        else if (!strncmp(line, "delay=", 6)) NET_DELAY = atoi(line + 6);
        else if (!strncmp(line, "wait=", 5)) NET_WAIT = atoi(line + 5);
        else if (!strncmp(line, "remap=", 6)) {
            /* remap=119:273,115:274,... (vieraan nappi:pelin nappi); remap=none poistaa */
            char *q = line + 6;
            int a, b, used;
            nRemap = 0;
            while (nRemap < 32 && sscanf(q, "%d:%d%n", &a, &b, &used) == 2) {
                remapFrom[nRemap] = a; remapTo[nRemap] = b; nRemap++;
                q += used;
                if (*q == ',') q++;
            }
        }
        else if (!strncmp(line, "seed=", 5)) SEED = (unsigned)atoi(line + 5);
    }
    fclose(f);
}

static void load_replay(void) {
    FILE *f = fopen("gr_keys.txt", "r");
    char line[256], hex[64];
    unsigned idx;
    int key, val;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'K' && sscanf(line + 2, "%u %d %d", &idx, &key, &val) == 3) {
            add_k(idx, key, val);
        } else if (line[0] == 'E' && sscanf(line + 2, "%u %40s", &idx, hex) == 2 && strlen(hex) >= 40) {
            Uint8 b[20];
            for (int i = 0; i < 20; i++) {
                unsigned v = 0;
                char t[3] = {hex[i * 2], hex[i * 2 + 1], 0};
                sscanf(t, "%x", &v);
                b[i] = (Uint8)v;
            }
            add_e(idx, b);
        }
    }
    fclose(f);
}

/* ---- IAT-koukut: haetaan pääohjelman tuonneista funktio nimellä ---- */
static int hook_iat(const char *fn, void *nf, void **orig) {
    BYTE *b = (BYTE *)GetModuleHandleA(NULL);
    DWORD lfanew = *(DWORD *)(b + 0x3C);
    BYTE *nt = b + lfanew;
    DWORD rva = *(DWORD *)(nt + 128); /* tuontitaulun RVA (PE32) */
    if (!rva) return 0;
    DWORD *d = (DWORD *)(b + rva);
    for (; d[3]; d += 5) { /* Name != 0 */
        DWORD *ot = (DWORD *)(b + (d[0] ? d[0] : d[4]));
        DWORD *ft = (DWORD *)(b + d[4]);
        for (; *ot; ot++, ft++) {
            if (*ot & 0x80000000u) continue;
            const char *nm = (const char *)(b + *ot + 2);
            if (strcmp(nm, fn) == 0) {
                DWORD old;
                VirtualProtect(ft, sizeof(DWORD), PAGE_READWRITE, &old);
                if (orig) *orig = (void *)(*ft);
                *ft = (DWORD)nf;
                VirtualProtect(ft, sizeof(DWORD), old, &old);
                return 1;
            }
        }
    }
    return 0;
}

static void (*o_srand)(unsigned);
static void h_srand(unsigned s) {
    cSrand++;
    if (LG) { fprintf(LG, "srand(%u) -> kaytetaan %u\n", s, SEED ? SEED : s); fflush(LG); }
    o_srand(SEED ? SEED : s);
}
static int (*o_rand)(void);
static int h_rand(void) { cRand++; totRand++; return o_rand(); }
static long long (*o_time64)(long long *);
static long long h_time64(long long *t) { cTime++; return o_time64(t); }
static int(__stdcall *o_qpc)(void *);
static int __stdcall h_qpc(void *p) { cQpc++; return o_qpc(p); }
static void(__stdcall *o_gst)(void *);
static void __stdcall h_gst(void *p) { cGst++; o_gst(p); }
static void(__stdcall *o_gstft)(void *);
static void __stdcall h_gstft(void *p) { cGstft++; o_gstft(p); }

static void init(void) {
    char path[MAX_PATH];
    DWORD err1 = 0, err2 = 0;
    if (R) return;
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
    if (R) {
        r_GetTicks = (Uint32(*)(void))GetProcAddress(R, "SDL_GetTicks");
        r_Delay = (void (*)(Uint32))GetProcAddress(R, "SDL_Delay");
        r_GetKeyState = (Uint8 * (*)(int *)) GetProcAddress(R, "SDL_GetKeyState");
        r_PollEvent = (int (*)(void *))GetProcAddress(R, "SDL_PollEvent");
        r_PumpEvents = (void (*)(void))GetProcAddress(R, "SDL_PumpEvents");
        r_Flip = (int (*)(void *))GetProcAddress(R, "SDL_Flip");
        r_OpenAudio = (int (*)(void *, void *))GetProcAddress(R, "SDL_OpenAudio");
    }
    load_cfg();
    if (MODE == 3) { /* auto: jos gr_keys.txt on olemassa, toista; muuten nauhoita */
        FILE *t = fopen("gr_keys.txt", "r");
        if (t) { fclose(t); MODE = 2; } else MODE = 1;
    }
    if (MODE == 4) {
        if (LG) { fprintf(LG, "net: yhdistetaan, huone=%s valittaja=%s:%d viive=%d tikkia\n", NET_ROOM, NET_BROKER, NET_PORT, NET_DELAY); fflush(LG); }
        if (!net_start(NET_BROKER, NET_PORT, NET_ROOM, NET_DELAY, LG, NET_WAIT * 1000)) {
            if (LG) { fprintf(LG, "net: yhteys epaonnistui -> offline\n"); fflush(LG); }
            MODE = 0;
        } else { nextTickV = V + 16; }
    }
    int h1 = hook_iat("srand", (void *)h_srand, (void **)&o_srand);
    int h2 = hook_iat("rand", (void *)h_rand, (void **)&o_rand);
    int h3 = hook_iat("_time64", (void *)h_time64, (void **)&o_time64);
    int h4 = hook_iat("QueryPerformanceCounter", (void *)h_qpc, (void **)&o_qpc);
    int h5 = hook_iat("GetSystemTime", (void *)h_gst, (void **)&o_gst);
    int h6 = hook_iat("GetSystemTimeAsFileTime", (void *)h_gstft, (void **)&o_gstft);
    if (MODE == 1) KF = fopen("gr_keys.txt", "w");
    if (MODE == 2) load_replay();
    TR = fopen(MODE == 2 ? "gr_trace_rep.txt" : (MODE == 4 ? "gr_trace_net.txt" : "gr_trace_rec.txt"), "w");
    if (LG) {
        fprintf(LG, "tried path: %s | error1=%lu error2=%lu\n", path, (unsigned long)err1, (unsigned long)err2);
        fprintf(LG, "proxy start v5 (verkko, audio=%s), real SDL loaded: %s | mode=%d vclock=%d seed=%u\n", AUDIO_OFF ? "off" : "on", R ? "yes" : "NO", MODE, VCLOCK, SEED);
        fprintf(LG, "hooks srand=%d rand=%d _time64=%d QPC=%d GetSystemTime=%d GetSystemTimeAsFileTime=%d\n", h1, h2, h3, h4, h5, h6);
        if (MODE == 2) fprintf(LG, "replay loaded: %u key records, %u event records\n", nk, ne);
        fflush(LG);
    }
    lastRep = GetTickCount();
}

static void report(void) {
    DWORD n = GetTickCount();
    if (n - lastRep < 1000 || !LG) return;
    fprintf(LG,
            "[sec] poll#=%u V=%u | GetTicks=%u Delay=%u GetKeyState=%u PollEvent=%u Flip=%u"
            " | srand=%u rand=%u time64=%u QPC=%u GetSysTime=%u GetSysTimeFT=%u\n",
            pollIdx, V, cGetTicks, cDelay, cGetKey, cPoll, cFlip, cSrand, cRand, cTime, cQpc, cGst, cGstft);
    fflush(LG);
    if (KF) fflush(KF);
    if (TR) fflush(TR);
    cGetTicks = cDelay = cGetKey = cPoll = cPump = cFlip = 0;
    if (MODE == 4) fprintf(LG, "net: tikki=%u stalls=%u rx=%u tx=%u\n", netTick, n_stalls, n_rx_msgs, n_tx_msgs);
    cSrand = cRand = cTime = cQpc = cGst = cGstft = 0;
    lastRep = n;
}

/* ---- verkkopeli (lockstep) ---- */
static int net_idle(void *u) {
    Uint8 ev[32];
    (void)u;
    r_PumpEvents();
    while (r_PollEvent(ev)) {
        if (ev[0] == 12) ExitProcess(0);
        if (ev[0] != 1 && ev[0] != 17 && carry.nev < NET_MAXE) memcpy(carry.ev[carry.nev++], ev, 20);
    }
    return 0;
}

static void net_sample(NIn *o) {
    int n0 = 0, i;
    Uint8 ev[32];
    Uint8 *k;
    memset(o, 0, sizeof *o);
    r_PumpEvents();
    k = r_GetKeyState(&n0);
    if (k) {
        if (n0 > 512) n0 = 512;
        for (i = 0; i < n0 && o->nk < NET_MAXK; i++) if (k[i]) o->k[o->nk++] = (unsigned short)i;
    }
    for (i = 0; i < carry.nev; i++) memcpy(o->ev[o->nev++], carry.ev[i], 20);
    carry.nev = 0;
    while (r_PollEvent(ev)) {
        if (ev[0] == 1 || ev[0] == 17) continue;
        if (o->nev >= NET_MAXE) continue;
        if (ev[0] == 4 && o->nev >= NET_MAXE - 2) continue;
        memcpy(o->ev[o->nev++], ev, 20);
    }
}

static void net_apply(const NIn *a, const NIn *b) {
    int i, j;
    const NIn *x[2];
    x[0] = a; x[1] = b;
    memset(netKeys, 0, sizeof netKeys);
    for (j = 0; j < 2; j++) {
        for (i = 0; i < x[j]->nk; i++) {
            int kk = x[j]->k[i], m;
            if (j == 1) for (m = 0; m < nRemap; m++) if (remapFrom[m] == kk) { kk = remapTo[m]; break; }
            if (kk >= 0 && kk < 512) netKeys[kk] = 1;
        }
        for (i = 0; i < x[j]->nev; i++) {
            memcpy(evq[evt & 511], x[j]->ev[i], 20);
            evt++;
        }
    }
}

static void net_tick(void) {
    while (MODE == 4 && V >= nextTickV) {
        NIn sm, h, g;
        nextTickV += 16;
        netTick++;
        net_sample(&sm);
        if (net_step(netTick, &sm, &h, &g, net_idle, 0)) ExitProcess(0);
        net_apply(&h, &g);
    }
}

EXP Uint32 SDL_GetTicks(void) {
    init();
    S++;
    TRACE('T');
    cGetTicks++;
    if (!VCLOCK) return r_GetTicks();
    if (++stuck > 3000) { V += 1; stuck = 0; } /* suojaus: odotussilmukka ilman Delayta */
    if (MODE == 4) net_tick();
    return V;
}

EXP void SDL_Delay(Uint32 ms) {
    init();
    S++;
    TRACE('D');
    cDelay++;
    if (LG && delayLogged < 20) {
        fprintf(LG, "Delay(%u) at V=%u\n", ms, V);
        delayLogged++;
    }
    V += ms;
    stuck = 0;
    if (MODE == 4) net_tick();
    r_Delay(ms);
    report();
}

EXP Uint8 *SDL_GetKeyState(int *numkeys) {
    init();
    S++;
    TRACE('K');
    cGetKey++;
    pollIdx++;
    int n0 = 0;
    Uint8 *k;
    if (MODE == 4) {
        if (numkeys) *numkeys = 323;
        report();
        return netKeys;
    }
    k = r_GetKeyState(&n0);
    if (MODE == 2) {
        while (rk < nk && krec[rk].idx <= S) {
            if (krec[rk].key >= 0 && krec[rk].key < 512) repKeys[krec[rk].key] = (Uint8)krec[rk].val;
            rk++;
        }
        if (numkeys) *numkeys = 323;
        report();
        return repKeys;
    }
    if (numkeys) *numkeys = n0;
    if (k) {
        int n = (n0 > 0 && n0 <= 512) ? n0 : 323;
        for (int i = 0; i < n; i++) {
            if (k[i] != prevKeys[i]) {
                if (KF) fprintf(KF, "K %u %d %d\n", S, i, k[i]);
                if (LG) fprintf(LG, "key %d -> %d at poll %u S=%u\n", i, k[i], pollIdx, S);
                prevKeys[i] = k[i];
            }
        }
    }
    report();
    return k;
}

EXP int SDL_PollEvent(void *e) {
    init();
    S++;
    TRACE('P');
    cPoll++;
    int r;
    if (MODE == 4) {
        if (e && evh != evt) { memcpy(e, evq[evh & 511], 20); evh++; return 1; }
        return 0;
    }
    r = r_PollEvent(e);
    if (MODE == 2) {
        if (r && e && *(Uint8 *)e == 12) return 1; /* SDL_QUIT sallitaan aina */
        if (re < ne && erec[re].idx <= S && e) {
            memcpy(e, erec[re].b, 20);
            re++;
            return 1;
        }
        return 0;
    }
    if (r && e && KF) {
        fprintf(KF, "E %u ", S);
        for (int i = 0; i < 20; i++) fprintf(KF, "%02x", ((Uint8 *)e)[i]);
        fprintf(KF, "\n");
    }
    return r;
}

EXP int SDL_OpenAudio(void *d, void *o) {
    init();
    if (AUDIO_OFF) { if (LG) { fprintf(LG, "SDL_OpenAudio estetty (audio=off)\n"); fflush(LG); } return -1; }
    return r_OpenAudio(d, o);
}

EXP void SDL_PumpEvents(void) {
    init();
    S++;
    TRACE('U');
    cPump++;
    r_PumpEvents();
}

static Uint32 fnv(const Uint8 *p, unsigned n, Uint32 h) {
    for (unsigned i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

EXP int SDL_Flip(void *s) {
    init();
    S++;
    TRACE('F');
    cFlip++;
    frameNo++;
    if (LG && s && (frameNo % 5) == 0) {
        Uint8 *sb = (Uint8 *)s;
        Uint8 *fmt = *(Uint8 **)(sb + 4);
        int w = *(int *)(sb + 8), h = *(int *)(sb + 12);
        unsigned pitch = *(unsigned short *)(sb + 16);
        Uint8 *px = *(Uint8 **)(sb + 20);
        if (fmt && px && w > 0 && h > 0 && w < 4096 && h < 4096) {
            unsigned bpp = fmt[5];
            Uint32 hh = 2166136261u;
            for (int y = 0; y < h; y += 3) hh = fnv(px + (unsigned)y * pitch, (unsigned)w * bpp, hh);
            fprintf(LG, "H frame=%u poll=%u S=%u hash=%08x\n", frameNo, pollIdx, S, hh);
        }
    }
    return r_Flip(s);
}
