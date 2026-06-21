#include "conn_state.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "cJSON.h"

#define CONN_MAX_SUBS 8

typedef struct { conn_state_cb cb; void *ud; bool used; } conn_sub_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    conn_state_t    cur;
    conn_sub_t      subs[CONN_MAX_SUBS];
    pthread_t       thread;
    bool            started;
    bool            stop;
    char            base_url[128];
    int             interval_ms;
} C = {
    .mu  = PTHREAD_MUTEX_INITIALIZER,
    .cv  = PTHREAD_COND_INITIALIZER,
    .cur = { CONN_UNKNOWN, "", -1, 0 },
};

static uint64_t conn_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

bool conn_state_parse(const char *json, conn_state_t *out) {
    out->state = CONN_UNKNOWN;
    out->reason[0] = '\0';
    out->since_ms = -1;
    out->updated_ms = 0;
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *conn = cJSON_GetObjectItemCaseSensitive(root, "connection");
    cJSON *st   = conn ? cJSON_GetObjectItemCaseSensitive(conn, "state") : NULL;
    bool ok = false;
    if (st && cJSON_IsString(st) && st->valuestring) {
        const char *s = st->valuestring;
        if      (strcmp(s, "connected")    == 0) { out->state = CONN_CONNECTED;    ok = true; }
        else if (strcmp(s, "armed")        == 0) { out->state = CONN_ARMED;        ok = true; }
        else if (strcmp(s, "disconnected") == 0) { out->state = CONN_DISCONNECTED; ok = true; }
        if (ok) {
            cJSON *rs = cJSON_GetObjectItemCaseSensitive(conn, "reason");
            if (rs && cJSON_IsString(rs) && rs->valuestring) {
                strncpy(out->reason, rs->valuestring, sizeof out->reason - 1);
                out->reason[sizeof out->reason - 1] = '\0';
            }
            cJSON *sm = cJSON_GetObjectItemCaseSensitive(conn, "sinceMs");
            if (sm && cJSON_IsNumber(sm)) out->since_ms = (long)sm->valuedouble;
        }
    }
    cJSON_Delete(root);
    return ok;
}

conn_state_t conn_state_get(void) {
    pthread_mutex_lock(&C.mu);
    conn_state_t c = C.cur;
    pthread_mutex_unlock(&C.mu);
    return c;
}

/* Apply a new snapshot; notify subscribers iff the enum state changed.
 * Subscribers are snapshotted under the lock then invoked unlocked. */
static void conn_state_apply(const conn_state_t *ns) {
    conn_sub_t snap[CONN_MAX_SUBS];
    int n = 0;
    bool changed;
    conn_state_t cur_copy;
    pthread_mutex_lock(&C.mu);
    changed = (ns->state != C.cur.state);
    C.cur = *ns;
    cur_copy = C.cur;
    if (changed)
        for (int i = 0; i < CONN_MAX_SUBS; i++)
            if (C.subs[i].used) snap[n++] = C.subs[i];
    pthread_mutex_unlock(&C.mu);
    for (int i = 0; i < n; i++) snap[i].cb(&cur_copy, snap[i].ud);
}

void conn_state_ingest(conn_state_e state, const char *reason, long since_ms) {
    conn_state_t ns;
    ns.state = state;
    ns.reason[0] = '\0';
    if (reason) { strncpy(ns.reason, reason, sizeof ns.reason - 1);
                  ns.reason[sizeof ns.reason - 1] = '\0'; }
    ns.since_ms = since_ms;
    ns.updated_ms = conn_now_ms();
    conn_state_apply(&ns);
}

int conn_state_subscribe(conn_state_cb cb, void *ud) {
    if (!cb) return -1;
    int token = -1;
    conn_state_t cur_copy;
    pthread_mutex_lock(&C.mu);
    for (int i = 0; i < CONN_MAX_SUBS; i++) {
        if (!C.subs[i].used) {
            C.subs[i].cb = cb; C.subs[i].ud = ud; C.subs[i].used = true;
            token = i; break;
        }
    }
    cur_copy = C.cur;
    pthread_mutex_unlock(&C.mu);
    if (token >= 0) cb(&cur_copy, ud);   /* immediate current-state delivery */
    return token;
}

void conn_state_unsubscribe(int token) {
    if (token < 0 || token >= CONN_MAX_SUBS) return;
    pthread_mutex_lock(&C.mu);
    C.subs[token].used = false;
    C.subs[token].cb = NULL;
    C.subs[token].ud = NULL;
    pthread_mutex_unlock(&C.mu);
}

void conn_state_start(const char *base_url, int interval_ms) { (void)base_url; (void)interval_ms; }
void conn_state_stop(void) {
    pthread_mutex_lock(&C.mu);
    /* Task 3 adds: stop+join the poller thread here when started. */
    for (int i = 0; i < CONN_MAX_SUBS; i++) {
        C.subs[i].used = false; C.subs[i].cb = NULL; C.subs[i].ud = NULL;
    }
    C.cur.state = CONN_UNKNOWN; C.cur.reason[0] = '\0'; C.cur.since_ms = -1; C.cur.updated_ms = 0;
    pthread_mutex_unlock(&C.mu);
}
