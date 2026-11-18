#ifndef LLM_H
#define LLM_H
typedef struct llm llm_t;
typedef int (*llm_token_cb)(void *ud, const char *piece, int len); // return nonzero to stop
llm_t *llm_load(const char *model_path, const char *tok_path, int max_seq);
int llm_encode(llm_t *m, const char *text, int *out, int max);
const char *llm_token_str(llm_t *m, int id, int *len);
int llm_chat_prefix(llm_t *m, const char *system, int *out, int max);
int llm_chat_user(llm_t *m, const char *user, int *out, int max);
void llm_set_pos(llm_t *m, int pos);
void llm_advise(llm_t *m, int need); // page in (1) / drop (0) weights
void llm_prefetch(llm_t *m);           // read the weights a forward pass needs into the page cache, sequentially
int llm_get_pos(llm_t *m);
int llm_feed(llm_t *m, const int *toks, int n);          // prefill (batched)
int llm_generate(llm_t *m, int maxnew, float rep_penalty, llm_token_cb cb, void *ud);
void llm_stats(llm_t *m, double *prefill_ms, int *nprefill, double *gen_ms, int *ngen);
#endif
