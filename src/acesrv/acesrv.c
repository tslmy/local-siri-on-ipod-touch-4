// acesrv: a minimal on-device "Siri server" (ACE protocol) for iOS 6 Siri clients.
// Original code; protocol behaviour follows the public SiriServerCore project
// (https://github.com/janrueth/SiriServerCore). Speech recognition and answers come
// from localsirid (127.0.0.1:7788), so everything stays on the device.
//
// Transport is plain TCP on loopback: assistantd accepts an http:// "Server Override" (it
// calls it an "insecure ace server"), so no certificate is involved.
//
// Wire format: "ACE /ace HTTP/1.0" header, then the bytes AA CC EE 02, then a zlib
// stream in each direction carrying chunks: 0x02 <u32be len> <binary plist> (object),
// 0x03 <u32be n> (ping), 0x04 <u32be n> (pong).
//
// Usage: acesrv [-p port] [-a bind_addr] [-v]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <zlib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <speex/speex.h>
#include <pthread.h>

static int verbose = 0;
static int cfd;
// Answers are computed on a worker thread so the main loop keeps answering keep-alive pings
// (assistantd drops the connection after a few seconds of silence). Sends are serialized, and
// a reply for a connection that has since closed is dropped (conn_gen).
static pthread_mutex_t send_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int conn_gen, conn_open;

static void logf_(const char *fmt, ...) {
    struct timeval tv; gettimeofday(&tv, 0);
    fprintf(stderr, "[acesrv %ld.%03d] ", (long)tv.tv_sec % 100000, (int)(tv.tv_usec / 1000));
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

/* ---------------- transport ---------------- */
static int net_read(void *b, int n) { return (int)read(cfd, b, n); }
static int net_write(const void *b, int n) {
    const uint8_t *p = b; int left = n;
    while (left > 0) {
        int w = (int)write(cfd, p, left);
        if (w <= 0) return -1;
        p += w; left -= w;
    }
    return n;
}

static z_stream zin, zout;
static uint8_t *inbuf; static size_t inlen, incap;   // decompressed, unparsed input

static void send_raw_chunk(const uint8_t *data, size_t n) {
    pthread_mutex_lock(&send_mu);
    if (!conn_open) { pthread_mutex_unlock(&send_mu); return; }
    uint8_t out[16384];
    zout.next_in = (Bytef *)data; zout.avail_in = (uInt)n;
    do {
        zout.next_out = out; zout.avail_out = sizeof out;
        deflate(&zout, Z_SYNC_FLUSH);
        net_write(out, (int)(sizeof out - zout.avail_out));
    } while (zout.avail_out == 0);
    pthread_mutex_unlock(&send_mu);
}

/* ---------------- plist helpers ---------------- */
static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static void dset(CFMutableDictionaryRef d, const char *k, CFTypeRef v) { CFStringRef ks = S(k); CFDictionarySetValue(d, ks, v); CFRelease(ks); CFRelease(v); }
static void dset_str(CFMutableDictionaryRef d, const char *k, const char *v) { dset(d, k, S(v)); }
static void dset_bool(CFMutableDictionaryRef d, const char *k, int v) { CFStringRef ks = S(k); CFDictionarySetValue(d, ks, v ? kCFBooleanTrue : kCFBooleanFalse); CFRelease(ks); }
static void dset_int(CFMutableDictionaryRef d, const char *k, int v) { dset(d, k, CFNumberCreate(NULL, kCFNumberIntType, &v)); }
static CFMutableDictionaryRef dnew(void) { return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); }
static CFMutableArrayRef anew(void) { return CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks); }
static void apush(CFMutableArrayRef a, CFTypeRef v) { CFArrayAppendValue(a, v); CFRelease(v); }

static void new_uuid(char out[37]) {
    CFUUIDRef u = CFUUIDCreate(NULL); CFStringRef s = CFUUIDCreateString(NULL, u);
    CFStringGetCString(s, out, 37, kCFStringEncodingUTF8); CFRelease(s); CFRelease(u);
}
static const char *dget_cstr(CFDictionaryRef d, const char *k, char *buf, int n) {
    if (!d) return NULL;
    CFStringRef ks = S(k); CFTypeRef v = CFDictionaryGetValue(d, ks); CFRelease(ks);
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return NULL;
    return CFStringGetCString(v, buf, n, kCFStringEncodingUTF8) ? buf : NULL;
}
static CFTypeRef dget(CFDictionaryRef d, const char *k) {
    if (!d) return NULL;
    CFStringRef ks = S(k); CFTypeRef v = CFDictionaryGetValue(d, ks); CFRelease(ks); return v;
}

// An embedded ACE object: {class, group, properties}
static CFMutableDictionaryRef ace_obj(const char *cls, const char *group, CFMutableDictionaryRef *props) {
    CFMutableDictionaryRef o = dnew();
    dset_str(o, "class", cls); dset_str(o, "group", group);
    *props = dnew();
    CFStringRef ks = S("properties"); CFDictionarySetValue(o, ks, *props); CFRelease(ks); CFRelease(*props);
    return o;
}
// A client-bound command: adds aceId and refId.
static CFMutableDictionaryRef ace_cmd(const char *cls, const char *group, const char *refId, CFMutableDictionaryRef *props) {
    CFMutableDictionaryRef o = ace_obj(cls, group, props);
    char id[37]; new_uuid(id); dset_str(o, "aceId", id);
    if (refId) dset_str(o, "refId", refId);
    return o;
}

static void send_obj(CFDictionaryRef o) {
    CFErrorRef err = NULL;
    CFDataRef d = CFPropertyListCreateData(NULL, o, kCFPropertyListBinaryFormat_v1_0, 0, &err);
    if (!d) { logf_("plist encode failed"); return; }
    size_t n = CFDataGetLength(d);
    uint8_t *b = malloc(n + 5);
    b[0] = 2; b[1] = n >> 24; b[2] = n >> 16; b[3] = n >> 8; b[4] = n;
    memcpy(b + 5, CFDataGetBytePtr(d), n);
    send_raw_chunk(b, n + 5);
    if (verbose) { char c[64] = "?"; dget_cstr(o, "class", c, sizeof c); logf_("-> %s (%zu bytes)", c, n); }
    free(b); CFRelease(d);
}
static void send_simple(const char *cls, const char *group, const char *refId) {
    CFMutableDictionaryRef p, o = ace_cmd(cls, group, refId, &p);
    send_obj(o); CFRelease(o);
}

/* ---------------- speech state ---------------- */
static struct { char id[64]; int active, speex, rate, decided; void *dec; SpeexBits bits; int fsz;
                int16_t *pcm; size_t n, cap; } sp;

static void pcm_append(const int16_t *s, size_t n) {
    if (sp.n + n > sp.cap) { sp.cap = (sp.n + n) * 2 + 16000; sp.pcm = realloc(sp.pcm, sp.cap * 2); }
    memcpy(sp.pcm + sp.n, s, n * 2); sp.n += n;
}
static void speech_reset(void) {
    if (sp.dec) { speex_decoder_destroy(sp.dec); speex_bits_destroy(&sp.bits); sp.dec = NULL; }
    sp.active = 0; sp.n = 0;
}
// Tell localsirid to load the speech model while the user is still talking.
static void *warm_backend(void *arg) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(7788); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        uint32_t z = 0; char ok[4];
        write(s, "WRM1", 4); write(s, &z, 4); read(s, ok, sizeof ok);
    }
    close(s);
    return NULL;
}

static void speech_start(const char *aceId, const char *codec) {
    pthread_t th;
    if (pthread_create(&th, NULL, warm_backend, NULL) == 0) pthread_detach(th);
    speech_reset();
    snprintf(sp.id, sizeof sp.id, "%s", aceId);
    sp.active = 1; sp.speex = 0; sp.rate = 16000; sp.decided = 0;
    if (codec && strstr(codec, "Speex")) {
        sp.speex = 1;
        int wb = strstr(codec, "WB") != NULL;
        sp.dec = speex_decoder_init(speex_lib_get_mode(wb ? SPEEX_MODEID_WB : SPEEX_MODEID_NB));
        int enh = 1; speex_decoder_ctl(sp.dec, SPEEX_SET_ENH, &enh);
        speex_decoder_ctl(sp.dec, SPEEX_GET_FRAME_SIZE, &sp.fsz);
        speex_bits_init(&sp.bits);
        sp.rate = wb ? 16000 : 8000;
    } else if (codec && strstr(codec, "PCM_Mono_16Bit_")) {
        sp.rate = atoi(strstr(codec, "16Bit_") + 6);
    }
    logf_("speech start %s codec=%s rate=%d", aceId, codec ? codec : "?", sp.rate);
}
static void speech_packets(CFDictionaryRef props) {
    if (!sp.active) return;
    CFTypeRef pk = dget(props, "packets");
    if (pk && CFGetTypeID(pk) == CFArrayGetTypeID()) {
        for (CFIndex i = 0; i < CFArrayGetCount(pk); i++) {
            CFDataRef d = CFArrayGetValueAtIndex(pk, i);
            if (CFGetTypeID(d) != CFDataGetTypeID()) continue;
            if (!sp.decided++ && sp.speex && CFDataGetLength(d) >= 320 && !(CFDataGetLength(d) & 1)) {
                // The capability tweak asks for 16 kHz PCM, but assistantd still labels the stream
                // Speex_WB (it maps the codec from its own settings), and sometimes the recording
                // really is Speex. Decide from the first packet only: Speex WB frames are ~70 bytes,
                // PCM buffers are hundreds. (Later packets can be large Speex flushes.)
                logf_("first packet is %ld bytes: treating stream as 16 kHz PCM", (long)CFDataGetLength(d));
                speex_decoder_destroy(sp.dec); speex_bits_destroy(&sp.bits); sp.dec = NULL;
                sp.speex = 0; sp.rate = 16000;
            }
            if (sp.speex) {
                int16_t fr[640];
                speex_bits_read_from(&sp.bits, (char *)CFDataGetBytePtr(d), (int)CFDataGetLength(d));
                while (speex_decode_int(sp.dec, &sp.bits, fr) == 0) pcm_append(fr, sp.fsz);
            } else pcm_append((const int16_t *)CFDataGetBytePtr(d), CFDataGetLength(d) / 2);
        }
    }
    CFTypeRef dd = dget(props, "data");
    if (!sp.speex && dd && CFGetTypeID(dd) == CFDataGetTypeID())
        pcm_append((const int16_t *)CFDataGetBytePtr(dd), CFDataGetLength(dd) / 2);
}

/* ---------------- localsirid bridge ---------------- */
// Sends audio (16 kHz) or text; fills heard/answer. Returns 0 on success.
static int ask_backend_cb(const int16_t *pcm, size_t n, const char *text, char *heard, int hn, char *answer, int an,
                          void (*on_heard)(const char *, void *), void *ctx) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(7788); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    uint32_t len;
    if (text) { len = (uint32_t)strlen(text); write(s, "TXT1", 4); write(s, &len, 4); write(s, text, len); }
    else { len = (uint32_t)n; write(s, "AUD1", 4); write(s, &len, 4); write(s, pcm, n * 2); }
    heard[0] = answer[0] = 0;
    char buf[8192]; size_t bl = 0; ssize_t r; int al = 0;
    while ((r = read(s, buf + bl, sizeof buf - 1 - bl)) > 0) {
        bl += r; buf[bl] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            if (!strncmp(line, "HEARD\t", 6)) { snprintf(heard, hn, "%s", line + 6); if (on_heard) on_heard(heard, ctx); }
            else if (!strncmp(line, "TOK\t", 4)) {
                const char *t = line + 4;
                for (; *t && al < an - 1; t++) {
                    if (t[0] == '\\' && t[1] == 'n') { answer[al++] = ' '; t++; }
                    else answer[al++] = *t;
                }
                answer[al] = 0;
            }
            line = nl + 1;
        }
        bl = strlen(line); memmove(buf, line, bl);
    }
    close(s);
    // trim
    char *p = answer; while (*p == ' ') p++;
    memmove(answer, p, strlen(p) + 1);
    for (int i = (int)strlen(answer) - 1; i >= 0 && (answer[i] == ' ' || answer[i] == '\n'); i--) answer[i] = 0;
    return 0;
}

static int ask_backend(const int16_t *pcm, size_t n, const char *text, char *heard, int hn, char *answer, int an) {
    return ask_backend_cb(pcm, n, text, heard, hn, answer, an, NULL, NULL);
}

static void send_recognized(const char *refId, const char *heard) {
    CFMutableDictionaryRef p, o;
    {
        // SpeechRecognized{recognition: Recognition{phrases:[Phrase{interpretations:[Interpretation{tokens}]}]}}
        CFMutableArrayRef tokens = anew();
        char w[1024]; snprintf(w, sizeof w, "%s", heard);
        int first = 1;
        for (char *tok = strtok(w, " "); tok; tok = strtok(NULL, " ")) {
            CFMutableDictionaryRef tp, t = ace_obj("Token", "com.apple.ace.speech", &tp);
            dset_str(tp, "text", tok); dset_int(tp, "startTime", 0); dset_int(tp, "endTime", 0);
            dset_int(tp, "confidenceScore", 1000); dset_bool(tp, "removeSpaceBefore", first); dset_bool(tp, "removeSpaceAfter", 0);
            apush(tokens, t); first = 0;
        }
        CFMutableDictionaryRef ip, interp = ace_obj("Interpretation", "com.apple.ace.speech", &ip);
        dset(ip, "tokens", tokens);
        CFMutableArrayRef interps = anew(); apush(interps, interp);
        CFMutableDictionaryRef pp, phrase = ace_obj("Phrase", "com.apple.ace.speech", &pp);
        dset_bool(pp, "lowConfidence", 0); dset(pp, "interpretations", interps);
        CFMutableArrayRef phrases = anew(); apush(phrases, phrase);
        CFMutableDictionaryRef rp, rec = ace_obj("Recognition", "com.apple.ace.speech", &rp);
        dset(rp, "phrases", phrases);
        o = ace_cmd("SpeechRecognized", "com.apple.ace.speech", refId, &p);
        dset(p, "recognition", rec);
        char sid[37]; new_uuid(sid); dset_str(p, "sessionId", sid);
        send_obj(o); CFRelease(o);
    }
}

static void reply_with_answer(const char *refId, const char *heard, const char *answer) {
    CFMutableDictionaryRef p, o;
    if (heard) send_recognized(refId, heard);
    // AddViews{views:[AssistantUtteranceView{text, speakableText}]}
    CFMutableDictionaryRef vp, view = ace_obj("AssistantUtteranceView", "com.apple.ace.assistant", &vp);
    dset_str(vp, "text", answer); dset_str(vp, "speakableText", answer);
    dset_str(vp, "dialogIdentifier", "Misc#ident"); dset_bool(vp, "listenAfterSpeaking", 0);
    char vid[37]; new_uuid(vid); dset_str(vp, "viewId", vid);
    CFMutableArrayRef views = anew(); apush(views, view);
    o = ace_cmd("AddViews", "com.apple.ace.assistant", refId, &p);
    dset_str(p, "dialogPhase", "Completion"); dset_bool(p, "scrollToTop", 0); dset_bool(p, "temporary", 0);
    dset(p, "views", views);
    send_obj(o); CFRelease(o);
    send_simple("RequestCompleted", "com.apple.ace.system", refId);
}

// One answer at a time: if Siri resends the request (e.g. after reconnecting), the new request
// adopts the job already running instead of starting a second, memory-hungry one.
struct job { char refId[96]; int16_t *pcm; size_t n; int gen; int sent_heard; char heard[1024]; };
static struct job *running;
static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;
static int job_live(struct job *j) { return j->gen == conn_gen && conn_open; }
static void job_heard(const char *heard, void *ctx) {
    struct job *j = ctx;
    snprintf(j->heard, sizeof j->heard, "%s", heard);
    if (heard[0] && job_live(j)) { send_recognized(j->refId, heard); j->sent_heard = 1; }   // show the words right away
}
static void *backend_job(void *arg) {
    struct job *j = arg;
    char heard[1024], answer[4096];
    int err = ask_backend_cb(j->pcm, j->n, NULL, heard, sizeof heard, answer, sizeof answer, job_heard, j);
    if (!job_live(j)) logf_("connection gone, dropping answer");
    else if (err) reply_with_answer(j->refId, NULL, "My on-device brain isn't running right now.");
    else if (!heard[0]) {
        CFMutableDictionaryRef p, o = ace_cmd("SpeechFailure", "com.apple.ace.speech", j->refId, &p);
        dset_str(p, "reason", "Inaudible"); dset_str(p, "reasonDescription", "No speech recognized");
        send_obj(o); CFRelease(o);
        send_simple("RequestCompleted", "com.apple.ace.system", j->refId);
    } else {
        logf_("heard \"%s\" -> \"%s\"", heard, answer);
        reply_with_answer(j->refId, j->sent_heard ? NULL : heard, answer[0] ? answer : "Hmm, I don't know.");
    }
    pthread_mutex_lock(&job_mu); if (running == j) running = NULL; pthread_mutex_unlock(&job_mu);
    free(j->pcm); free(j);
    return NULL;
}

static void speech_finish(const char *refId) {
    if (!sp.active) return;
    sp.active = 0;
    // resample to 16 kHz if needed (linear)
    int16_t *pcm = sp.pcm; size_t n = sp.n; int16_t *rs = NULL;
    if (sp.rate != 16000 && n) {
        size_t m = (size_t)((double)n * 16000 / sp.rate); rs = malloc(m * 2);
        for (size_t i = 0; i < m; i++) { double x = (double)i * sp.rate / 16000; size_t j = (size_t)x; double f = x - j;
            rs[i] = (int16_t)(j + 1 < n ? pcm[j] * (1 - f) + pcm[j + 1] * f : pcm[n - 1]); }
        pcm = rs; n = m;
    }
    // Test hook: if /tmp/acesrv-test.wav (16 kHz mono s16) exists, it replaces the mic audio.
    int16_t *tw = NULL;
    FILE *tf = fopen("/tmp/acesrv-test.wav", "rb");
    if (tf) {
        char id[4]; uint32_t sz; fseek(tf, 12, SEEK_SET);
        while (fread(id, 1, 4, tf) == 4 && fread(&sz, 4, 1, tf) == 1) {
            if (!memcmp(id, "data", 4)) {
                tw = malloc(sz); n = fread(tw, 2, sz / 2, tf); pcm = tw;
                logf_("test hook: using /tmp/acesrv-test.wav instead of the mic");
                break;
            }
            fseek(tf, sz, SEEK_CUR);
        }
        fclose(tf);
    }
    logf_("speech finish: %.2f s of audio -> localsirid", n / 16000.0);
    pthread_mutex_lock(&job_mu);
    if (running) {
        logf_("a job is already running; this request adopts it");
        snprintf(running->refId, sizeof running->refId, "%s", refId);
        running->gen = conn_gen; running->sent_heard = 0;
        if (running->heard[0]) { send_recognized(refId, running->heard); running->sent_heard = 1; }
        pthread_mutex_unlock(&job_mu);
        free(rs); free(tw); speech_reset();
        return;
    }
    struct job *j = calloc(1, sizeof *j);
    running = j;
    pthread_mutex_unlock(&job_mu);
    snprintf(j->refId, sizeof j->refId, "%s", refId);
    j->pcm = malloc(n * 2 + 2); memcpy(j->pcm, pcm, n * 2); j->n = n; j->gen = conn_gen;
    pthread_t th;
    if (pthread_create(&th, NULL, backend_job, j) == 0) pthread_detach(th);
    else backend_job(j);
    free(rs); free(tw);
    speech_reset();
}

/* ---------------- object dispatch ---------------- */
static void handle_obj(CFDictionaryRef o) {
    char cls[96] = "", aceId[96] = "", refId[96] = "", tmp[512];
    dget_cstr(o, "class", cls, sizeof cls); dget_cstr(o, "aceId", aceId, sizeof aceId); dget_cstr(o, "refId", refId, sizeof refId);
    CFDictionaryRef props = dget(o, "properties");
    if (props && CFGetTypeID(props) != CFDictionaryGetTypeID()) props = NULL;
    if (verbose) logf_("<- %s aceId=%s refId=%s", cls, aceId, refId);

    CFMutableDictionaryRef p, r;
    if (!strcmp(cls, "CreateAssistant")) {
        r = ace_cmd("AssistantCreated", "com.apple.ace.system", aceId, &p);
        char id[37]; new_uuid(id); dset_str(p, "assistantId", id);
        new_uuid(id); dset_str(p, "speechId", id);
        send_obj(r); CFRelease(r);
    } else if (!strcmp(cls, "LoadAssistant")) {
        r = ace_cmd("AssistantLoaded", "com.apple.ace.system", aceId, &p);
        dset_str(p, "version", "localsiri-1"); dset_bool(p, "requestSync", 0); dset_str(p, "dataAnchor", "localsiri");
        send_obj(r); CFRelease(r);
    } else if (!strcmp(cls, "DestroyAssistant")) {
        r = ace_cmd("AssistantDestroyed", "com.apple.ace.system", aceId, &p);
        if (dget_cstr(props, "assistantId", tmp, sizeof tmp)) dset_str(p, "assistantId", tmp);
        send_obj(r); CFRelease(r);
    } else if (!strcmp(cls, "CreateSessionInfoRequest")) {
        r = ace_cmd("CommandFailed", "com.apple.ace.system", aceId, &p);
        dset_str(p, "reason", "Not authenticated"); dset_int(p, "errorCode", 0);
        send_obj(r); CFRelease(r);
    } else if (!strcmp(cls, "StartSpeechRequest") || !strcmp(cls, "StartSpeechDictation")) {
        speech_start(aceId, dget_cstr(props, "codec", tmp, sizeof tmp));
    } else if (!strcmp(cls, "SpeechPacket")) {
        speech_packets(props);
    } else if (!strcmp(cls, "FinishSpeech")) {
        speech_finish(refId[0] ? refId : sp.id);
    } else if (!strcmp(cls, "StartRequest") || !strcmp(cls, "StartCorrectedSpeechRequest")) {
        if (dget_cstr(props, "utterance", tmp, sizeof tmp)) {
            char heard[8], answer[4096];
            if (ask_backend(NULL, 0, tmp, heard, sizeof heard, answer, sizeof answer)) snprintf(answer, sizeof answer, "My on-device brain isn't running right now.");
            reply_with_answer(aceId, NULL, answer[0] ? answer : "Hmm, I don't know.");
        }
    } else if (!strcmp(cls, "CancelRequest")) {
        speech_reset();
        send_simple("CancelSucceeded", "com.apple.ace.system", aceId);
    } else if (!strcmp(cls, "GetAnchors")) {
        // com.apple.ace.sync: we keep no synced data, so report no anchors; the client then
        // syncs whatever it wants to (or nothing) and carries on.
        r = ace_cmd("GetAnchorsResponse", "com.apple.ace.sync", aceId, &p);
        dset(p, "anchors", CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks));
        send_obj(r); CFRelease(r);
    } else if (!strcmp(cls, "Chunk")) {
        // accept every sync chunk, echoing its new generation as the current anchor
        CFMutableDictionaryRef ap, anchor = ace_obj("Anchor", "com.apple.ace.sync", &ap);
        char key[256] = "", gen[256] = "", val[256] = "";
        dget_cstr(props, "key", key, sizeof key); dget_cstr(props, "postGen", gen, sizeof gen);
        dget_cstr(props, "validity", val, sizeof val);
        dset_str(ap, "key", key); dset_str(ap, "generation", gen); dset_str(ap, "value", gen); dset_str(ap, "validity", val);
        r = ace_cmd("ChunkAccepted", "com.apple.ace.sync", aceId, &p);
        dset(p, "current", anchor);
        send_obj(r); CFRelease(r);
    } else if (!strcmp(cls, "GetSessionCertificate")) {
        r = ace_cmd("CommandFailed", "com.apple.ace.system", aceId, &p);
        dset_str(p, "reason", "Not supported"); dset_int(p, "errorCode", 0);
        send_obj(r); CFRelease(r);
    }
    // SetAssistantData, SetRequestOrigin, ClearContext, ... need no reply.
}

static void process_input(void) {
    for (;;) {
        if (inlen < 5) return;
        uint8_t t = inbuf[0];
        uint32_t v = (uint32_t)inbuf[1] << 24 | inbuf[2] << 16 | inbuf[3] << 8 | inbuf[4];
        size_t used;
        if (t == 3) {          // ping -> pong
            static uint32_t pong;
            uint8_t b[5] = {4, 0, 0, 0, 0}; pong++;
            b[1] = pong >> 24; b[2] = pong >> 16; b[3] = pong >> 8; b[4] = pong;
            send_raw_chunk(b, 5); used = 5;
        } else if (t == 4) used = 5;
        else if (t == 2) {
            if (inlen < 5 + (size_t)v) return;
            CFDataRef d = CFDataCreateWithBytesNoCopy(NULL, inbuf + 5, v, kCFAllocatorNull);
            CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, d, kCFPropertyListImmutable, NULL, NULL);
            CFRelease(d);
            if (pl && CFGetTypeID(pl) == CFDictionaryGetTypeID()) handle_obj(pl);
            else logf_("bad plist (%u bytes)", v);
            if (pl) CFRelease(pl);
            used = 5 + v;
        } else if (t == 0xff) { if (verbose) logf_("client sent end of stream"); inlen = 0; return; }
        else { logf_("unknown chunk type %d", t); inlen = 0; return; }
        memmove(inbuf, inbuf + used, inlen - used); inlen -= used;
    }
}

static void serve(void) {
    // HTTP-ish header
    char hdr[4096]; int hl = 0;
    while (hl < (int)sizeof hdr - 1) {
        int r = net_read(hdr + hl, 1);
        if (r <= 0) return;
        hl++; hdr[hl] = 0;
        if (hl >= 4 && !memcmp(hdr + hl - 4, "\r\n\r\n", 4)) break;
    }
    if (verbose) logf_("header: %.*s", (int)strcspn(hdr, "\r"), hdr);
    if (strncmp(hdr, "ACE /ace", 8)) {
        const char *m = "HTTP/1.0 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        net_write(m, (int)strlen(m)); return;
    }
    const char *ok = "HTTP/1.1 200 OK\r\nServer: localsiri\r\nConnection: close\r\n\r\n";
    net_write(ok, (int)strlen(ok));
    uint8_t magic[4]; int got = 0;
    while (got < 4) { int r = net_read(magic + got, 4 - got); if (r <= 0) return; got += r; }
    if (memcmp(magic, "\xaa\xcc\xee\x02", 4)) { logf_("bad stream magic"); return; }
    net_write("\xaa\xcc\xee\x02", 4);

    memset(&zin, 0, sizeof zin); memset(&zout, 0, sizeof zout);
    pthread_mutex_lock(&send_mu);
    inflateInit(&zin); deflateInit(&zout, Z_DEFAULT_COMPRESSION); conn_open = 1;
    pthread_mutex_unlock(&send_mu);
    uint8_t raw[16384], dec[65536];
    for (;;) {
        int r = net_read(raw, sizeof raw);
        if (r <= 0) break;
        zin.next_in = raw; zin.avail_in = r;
        do {
            zin.next_out = dec; zin.avail_out = sizeof dec;
            int zr = inflate(&zin, Z_SYNC_FLUSH);
            size_t n = sizeof dec - zin.avail_out;
            if (inlen + n > incap) { incap = (inlen + n) * 2; inbuf = realloc(inbuf, incap); }
            memcpy(inbuf + inlen, dec, n); inlen += n;
            if (zr != Z_OK && zr != Z_BUF_ERROR) { if (zr != Z_STREAM_END) logf_("inflate error %d", zr); break; }
        } while (zin.avail_out == 0);
        process_input();
    }
    pthread_mutex_lock(&send_mu); conn_open = 0; deflateEnd(&zout); pthread_mutex_unlock(&send_mu);
    inflateEnd(&zin); inlen = 0; speech_reset();
}

int main(int argc, char **argv) {
    int port = 4443; const char *addr = "127.0.0.1";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) addr = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = inet_addr(addr);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) { perror("bind"); return 1; }
    logf_("listening on %s:%d", addr, port);
    for (;;) {
        cfd = accept(s, NULL, NULL);
        if (cfd < 0) continue;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        logf_("client connected");
        pthread_mutex_lock(&send_mu); conn_gen++; pthread_mutex_unlock(&send_mu);
        serve();
        close(cfd);
        logf_("client disconnected");
    }
}
