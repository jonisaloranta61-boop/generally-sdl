/* net.h - GeneRally lockstep-verkko: MQTT-valittaja (huonekoodi = aihe), tikkikohtainen syötteiden vaihto.
   Siirrettävä: Winsock (Windows) ja POSIX (testit). */
#ifndef NET_H
#define NET_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET nsock;
#define NSOCK_BAD INVALID_SOCKET
#define nclose closesocket
static unsigned n_now(void) { return (unsigned)GetTickCount(); }
static void n_sleep(unsigned ms) { Sleep(ms); }
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
typedef int nsock;
#define NSOCK_BAD (-1)
#define nclose close
static unsigned n_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
static void n_sleep(unsigned ms) { usleep(ms * 1000); }
#endif

#define NET_RING 1024
#define NET_MASK (NET_RING - 1)
#define NET_MAXK 12
#define NET_MAXE 6

typedef struct {
    int nk;
    unsigned short k[NET_MAXK];
    int nev;
    unsigned char ev[NET_MAXE][20];
} NIn;
typedef struct { unsigned tick; int valid; NIn in; } NSlot;

static nsock n_s = NSOCK_BAD;
static unsigned char n_rb[70000];
static int n_rl;
static char n_host[96], n_room[48];
static int n_port = 1883;
static unsigned n_id, n_peer;
static int n_havePeer, n_peerSeen, n_dead;
static int n_delay = 8;
static FILE *n_log;
static NSlot n_loc[NET_RING], n_rem[NET_RING];
static unsigned n_ack, n_peerAck, n_lastPing, n_lastSub;
static unsigned n_stalls, n_rx_msgs, n_tx_msgs;

static void n_put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static unsigned n_get32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

static int n_send_all(const unsigned char *b, int len) {
    int off = 0;
    if (n_s == NSOCK_BAD) return -1;
    while (off < len) {
        int r = (int)send(n_s, (const char *)b + off, len - off, 0);
        if (r <= 0) { n_dead = 1; return -1; }
        off += r;
    }
    return 0;
}

/* MQTT 3.1.1, QoS 0 */
static int n_mqtt_pub(const char *topic, const unsigned char *pl, int pll) {
    unsigned char buf[20000];
    int tl = (int)strlen(topic), rl = 2 + tl + pll, i = 0;
    if (rl > 16000) return -1;
    buf[i++] = 0x30;
    if (rl < 128) buf[i++] = (unsigned char)rl;
    else { buf[i++] = (unsigned char)(rl % 128 | 128); buf[i++] = (unsigned char)(rl / 128); }
    buf[i++] = (unsigned char)(tl >> 8); buf[i++] = (unsigned char)tl;
    memcpy(buf + i, topic, tl); i += tl;
    memcpy(buf + i, pl, pll); i += pll;
    n_tx_msgs++;
    return n_send_all(buf, i);
}

static void n_my_topic(char *out) { sprintf(out, "grlock1/%s/%08x", n_room, n_id); }

static int n_recv_exact(unsigned char *b, int len, int to_ms) {
    int got = 0;
    unsigned t0 = n_now();
    while (got < len) {
        fd_set fs; struct timeval tv; int r;
        int left = to_ms - (int)(n_now() - t0);
        if (left <= 0) return -1;
        FD_ZERO(&fs); FD_SET(n_s, &fs);
        tv.tv_sec = left / 1000; tv.tv_usec = (left % 1000) * 1000;
        r = select((int)n_s + 1, &fs, 0, 0, &tv);
        if (r <= 0) continue;
        r = (int)recv(n_s, (char *)b + got, len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int n_connect(void) {
    struct hostent *he;
    struct sockaddr_in sa;
    unsigned char buf[256], ack[4];
    char cid[40], filt[96];
    int i = 0, rl, one = 1, fl, tl;
    n_rl = 0; n_dead = 0;
    if (n_s != NSOCK_BAD) { nclose(n_s); n_s = NSOCK_BAD; }
    he = gethostbyname(n_host);
    if (!he) { if (n_log) fprintf(n_log, "net: DNS epaonnistui (%s)\n", n_host); return -1; }
    n_s = socket(AF_INET, SOCK_STREAM, 0);
    if (n_s == NSOCK_BAD) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)n_port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    if (connect(n_s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (n_log) fprintf(n_log, "net: connect epaonnistui\n");
        nclose(n_s); n_s = NSOCK_BAD; return -1;
    }
    setsockopt(n_s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    sprintf(cid, "grl%08x", n_id);
    fl = 2 + 4 + 1 + 1 + 2 + 2 + (int)strlen(cid);
    buf[i++] = 0x10; buf[i++] = (unsigned char)fl;
    buf[i++] = 0; buf[i++] = 4; buf[i++] = 'M'; buf[i++] = 'Q'; buf[i++] = 'T'; buf[i++] = 'T';
    buf[i++] = 4; buf[i++] = 2; buf[i++] = 0; buf[i++] = 60;
    buf[i++] = 0; buf[i++] = (unsigned char)strlen(cid);
    memcpy(buf + i, cid, strlen(cid)); i += (int)strlen(cid);
    if (n_send_all(buf, i)) return -1;
    if (n_recv_exact(ack, 4, 8000) || ack[0] != 0x20 || ack[3] != 0) {
        if (n_log) fprintf(n_log, "net: CONNACK puuttuu/hylatty\n");
        return -1;
    }
    sprintf(filt, "grlock1/%s/+", n_room);
    tl = (int)strlen(filt);
    rl = 2 + 2 + tl + 1;
    i = 0;
    buf[i++] = 0x82; buf[i++] = (unsigned char)rl; buf[i++] = 0; buf[i++] = 1;
    buf[i++] = 0; buf[i++] = (unsigned char)tl;
    memcpy(buf + i, filt, tl); i += tl;
    buf[i++] = 0;
    if (n_send_all(buf, i)) return -1;
    n_lastPing = n_now();
    if (n_log) { fprintf(n_log, "net: yhdistetty %s:%d huone=%s id=%08x\n", n_host, n_port, n_room, n_id); fflush(n_log); }
    return 0;
}

static void n_advance_ack(void) {
    for (;;) {
        NSlot *s = &n_rem[n_ack & NET_MASK];
        if (s->valid && s->tick == n_ack) n_ack++;
        else break;
    }
}

static void n_on_pub(const unsigned char *p, int len) {
    if (len < 6) return;
    n_rx_msgs++;
    if (p[0] == 'H') {
        unsigned id = n_get32(p + 1);
        if (id == n_id) return;
        if (!n_havePeer || n_peer == id) { n_peer = id; n_havePeer = 1; if (p[5]) n_peerSeen = 1; }
    } else if (p[0] == 'I' && len >= 10) {
        unsigned id = n_get32(p + 1), ack;
        int cnt, pos = 10, e;
        if (id == n_id) return;
        if (!n_havePeer) { n_peer = id; n_havePeer = 1; }
        if (id != n_peer) return;
        n_peerSeen = 1;
        ack = n_get32(p + 5);
        if (ack > n_peerAck) n_peerAck = ack;
        cnt = p[9];
        for (e = 0; e < cnt; e++) {
            NIn in; unsigned tick; int k;
            memset(&in, 0, sizeof in);
            if (pos + 5 > len) return;
            tick = n_get32(p + pos); pos += 4;
            in.nk = p[pos++];
            if (in.nk > NET_MAXK || pos + in.nk * 2 + 1 > len) return;
            for (k = 0; k < in.nk; k++) { in.k[k] = (unsigned short)(p[pos] | (p[pos + 1] << 8)); pos += 2; }
            in.nev = p[pos++];
            if (in.nev > NET_MAXE || pos + in.nev * 20 > len) return;
            for (k = 0; k < in.nev; k++) { memcpy(in.ev[k], p + pos, 20); pos += 20; }
            if (tick >= n_ack && tick < n_ack + NET_RING) {
                NSlot *s = &n_rem[tick & NET_MASK];
                if (!(s->valid && s->tick == tick)) { s->tick = tick; s->valid = 1; s->in = in; }
            }
        }
        n_advance_ack();
    }
}

/* lukee/parsii saapuvaa dataa, odottaa enintaan waitms */
static void n_rx(int waitms) {
    fd_set fs; struct timeval tv; int r;
    if (n_s == NSOCK_BAD || n_dead) return;
    FD_ZERO(&fs); FD_SET(n_s, &fs);
    tv.tv_sec = waitms / 1000; tv.tv_usec = (waitms % 1000) * 1000;
    r = select((int)n_s + 1, &fs, 0, 0, &tv);
    if (r > 0) {
        r = (int)recv(n_s, (char *)n_rb + n_rl, (int)sizeof n_rb - n_rl, 0);
        if (r <= 0) { n_dead = 1; return; }
        n_rl += r;
    }
    for (;;) {
        int i = 1, mult = 1, rl = 0, more = 0;
        unsigned char b;
        if (n_rl < 2) break;
        do {
            if (i >= n_rl) { more = 1; break; }
            b = n_rb[i++]; rl += (b & 127) * mult; mult *= 128;
        } while ((b & 128) && i < 5);
        if (more || n_rl < i + rl) break;
        if ((n_rb[0] >> 4) == 3) {
            int qos = (n_rb[0] >> 1) & 3;
            int tl = (n_rb[i] << 8) | n_rb[i + 1];
            int off = i + 2 + tl + (qos ? 2 : 0);
            if (off <= i + rl) n_on_pub(n_rb + off, i + rl - off);
        }
        memmove(n_rb, n_rb + i + rl, n_rl - (i + rl));
        n_rl -= i + rl;
    }
}

static void n_send_hello(void) {
    unsigned char b[8]; char topic[96];
    b[0] = 'H'; n_put32(b + 1, n_id); b[5] = (unsigned char)n_havePeer;
    n_my_topic(topic);
    n_mqtt_pub(topic, b, 6);
}

static void n_send_I(unsigned upto) {
    unsigned char b[16000]; char topic[96];
    unsigned t, end;
    int pos = 10, cnt = 0;
    end = n_peerAck + 39; if (end > upto) end = upto;
    b[0] = 'I'; n_put32(b + 1, n_id); n_put32(b + 5, n_ack);
    for (t = n_peerAck; t <= end; t++) {
        NSlot *s = &n_loc[t & NET_MASK];
        int k;
        if (!(s->valid && s->tick == t)) continue;
        n_put32(b + pos, t); pos += 4;
        b[pos++] = (unsigned char)s->in.nk;
        for (k = 0; k < s->in.nk; k++) { b[pos++] = (unsigned char)s->in.k[k]; b[pos++] = (unsigned char)(s->in.k[k] >> 8); }
        b[pos++] = (unsigned char)s->in.nev;
        for (k = 0; k < s->in.nev; k++) { memcpy(b + pos, s->in.ev[k], 20); pos += 20; }
        cnt++;
    }
    b[9] = (unsigned char)cnt;
    n_my_topic(topic);
    n_mqtt_pub(topic, b, pos);
}

static void n_keepalive(void) {
    unsigned now = n_now();
    if (now - n_lastPing > 25000) {
        unsigned char p[2] = {0xC0, 0};
        n_send_all(p, 2);
        n_lastPing = now;
    }
}

/* palauttaa 1 jos valmis, 0 jos aikakatkaisu */
static int net_start(const char *host, int port, const char *room, int delay, FILE *log, int timeout_ms) {
    unsigned t0, lastH = 0, i;
#ifdef _WIN32
    WSADATA wd;
    WSAStartup(0x0202, &wd);
#endif
    n_log = log;
    strncpy(n_host, host, sizeof n_host - 1);
    strncpy(n_room, room, sizeof n_room - 1);
    n_port = port;
    n_delay = delay < 2 ? 2 : (delay > 60 ? 60 : delay);
    n_id = (n_now() * 2654435761u) ^ ((unsigned)time(0) * 40503u) ^ (unsigned)rand() ^ ((unsigned)rand() << 16) ^ (unsigned)(size_t)&t0;
    if (n_id == 0) n_id = 1;
    memset(n_loc, 0, sizeof n_loc); memset(n_rem, 0, sizeof n_rem);
    for (i = 0; i <= (unsigned)n_delay; i++) {
        n_loc[i].tick = i; n_loc[i].valid = 1; n_rem[i].tick = i; n_rem[i].valid = 1;
    }
    n_ack = n_peerAck = (unsigned)n_delay + 1;
    t0 = n_now();
    while ((int)(n_now() - t0) < timeout_ms) {
        if (n_s == NSOCK_BAD || n_dead) {
            if (n_connect()) { n_dead = 1; n_lastSub = n_now(); n_sleep(1500); continue; }
        }
        if (n_now() - lastH > 300) { n_send_hello(); lastH = n_now(); }
        n_rx(100);
        n_keepalive();
        if (n_havePeer && n_peerSeen) {
            if (n_log) { fprintf(n_log, "net: vastapelaaja loytyi (%08x), %s\n", n_peer, n_id < n_peer ? "olen isanta" : "olen vieras"); fflush(n_log); }
            return 1;
        }
    }
    return 0;
}

static int net_is_host(void) { return n_id < n_peer; }

/* tikki t: tallentaa oman naytteen tikille t+delay, odottaa vastapelaajan tikin t, palauttaa kummankin kasitellyn syotteen.
   idle() kutsutaan odotuksen aikana; jos palauttaa != 0, keskeytetaan (paluu -1). */
static int net_step(unsigned t, const NIn *samp, NIn *host, NIn *guest, int (*idle)(void *), void *ud) {
    NSlot *s = &n_loc[(t + n_delay) & NET_MASK];
    unsigned w0, lastTx, lastLog, spin = 0;
    NSlot *r;
    s->tick = t + n_delay; s->valid = 1; s->in = *samp;
    n_send_I(t + n_delay);
    n_rx(0);
    r = &n_rem[t & NET_MASK];
    w0 = lastTx = lastLog = n_now();
    while (!(r->valid && r->tick == t)) {
        unsigned now;
        if (spin++ == 0) n_stalls++;
        if (n_dead) {
            if (n_now() - n_lastSub > 1500) { n_lastSub = n_now(); if (n_connect() == 0) n_send_I(t + n_delay); }
        } else n_rx(10);
        now = n_now();
        if (now - lastTx > 200) { n_send_I(t + n_delay); lastTx = now; }
        n_keepalive();
        if (now - lastLog > 5000) {
            if (n_log) { fprintf(n_log, "net: odotetaan vastapelaajaa tikille %u (%u ms)\n", t, now - w0); fflush(n_log); }
            lastLog = now;
        }
        if (idle && idle(ud)) return -1;
    }
    {
        NIn *lo = &n_loc[t & NET_MASK].in, *re = &r->in;
        if (net_is_host()) { *host = *lo; *guest = *re; } else { *host = *re; *guest = *lo; }
    }
    return 0;
}

#endif
