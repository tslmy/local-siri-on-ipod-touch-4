#ifndef WHISTLE_H
#define WHISTLE_H
typedef struct whistle whistle_t;
whistle_t *whistle_load(const char *cact_path);   // the published whistle.cact, used in place
void whistle_advise(whistle_t *m, int need);       // page in (1) / drop (0) weights
void whistle_prefetch(whistle_t *m);               // read the weights into the page cache, sequentially
// audio: mono float [-1,1] at 16 kHz. Writes UTF-8 text (English), returns its length.
int whistle_transcribe(whistle_t *m, const float *audio, int n, char *out, int outlen);
void whistle_stats(whistle_t *m, double *feat_ms, double *enc_ms, double *dec_ms, int *ntok);
#endif
