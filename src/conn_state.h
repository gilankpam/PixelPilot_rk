#ifndef PP_CONN_STATE_H
#define PP_CONN_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONN_UNKNOWN = 0,     /* no data yet / fpvd or GS unreachable / .connection absent */
    CONN_DISCONNECTED,
    CONN_ARMED,
    CONN_CONNECTED,
} conn_state_e;

typedef struct {
    conn_state_e state;
    char         reason[64];   /* fpvd .reason; "" if none */
    long         since_ms;     /* .sinceMs, -1 if absent/null */
    uint64_t     updated_ms;   /* CLOCK_MONOTONIC ms of last apply */
} conn_state_t;

typedef void (*conn_state_cb)(const conn_state_t *st, void *ud);

/* Pure: parse a /gs/status body. On a recognized .connection.state, fills *out
 * and returns true; otherwise sets *out to {CONN_UNKNOWN,"",-1,0} and returns
 * false. Never reads the clock (updated_ms left 0). */
bool conn_state_parse(const char *json, conn_state_t *out);

/* Poller lifecycle. start() is idempotent; stop() also resets state +
 * subscribers (safe to call when never started — used by tests). */
void conn_state_start(const char *base_url, int interval_ms);
void conn_state_stop(void);

conn_state_t conn_state_get(void);

/* Feed a state from any source (poller, simulator, tests). Notifies
 * subscribers iff the enum state changed. */
void conn_state_ingest(conn_state_e state, const char *reason, long since_ms);

/* Returns a token >= 0, or -1 if full/invalid. Delivers the current snapshot
 * to cb once, synchronously, before returning. */
int  conn_state_subscribe(conn_state_cb cb, void *ud);
void conn_state_unsubscribe(int token);

#ifdef __cplusplus
}
#endif
#endif
