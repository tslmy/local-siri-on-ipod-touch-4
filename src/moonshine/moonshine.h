#ifndef MOONSHINE_H
#define MOONSHINE_H
typedef struct moonshine moonshine_t;
moonshine_t *moonshine_load(const char *model_path, const char *vocab_path);
void moonshine_free(moonshine_t *m);
void moonshine_advise(moonshine_t *m, int need); // page in (1) / drop (0) weights
void moonshine_prefetch(moonshine_t *m);          // read the weights into the page cache, sequentially
// audio: mono float [-1,1] at 16 kHz. Writes UTF-8 text, returns its length.
int moonshine_transcribe(moonshine_t *m, const float *audio, int n, char *out, int outlen);
void moonshine_stats(moonshine_t *m, double *enc_ms, double *dec_ms, int *ntok);
float *moonshine_read_wav(const char *path, int *nsamples); // resampled to 16 kHz mono
#endif
