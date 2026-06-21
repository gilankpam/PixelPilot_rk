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

/* Filled in Task 2. */
void conn_state_ingest(conn_state_e state, const char *reason, long since_ms) {
    pthread_mutex_lock(&C.mu);
    C.cur.state = state;
    C.cur.reason[0] = '\0';
    if (reason) { strncpy(C.cur.reason, reason, sizeof C.cur.reason - 1);
                  C.cur.reason[sizeof C.cur.reason - 1] = '\0'; }
    C.cur.since_ms = since_ms;
    C.cur.updated_ms = conn_now_ms();
    pthread_mutex_unlock(&C.mu);
}

int  conn_state_subscribe(conn_state_cb cb, void *ud) { (void)cb; (void)ud; return -1; }
void conn_state_unsubscribe(int token) { (void)token; }

void conn_state_start(const char *base_url, int interval_ms) { (void)base_url; (void)interval_ms; }
void conn_state_stop(void) {
    pthread_mutex_lock(&C.mu);
    C.cur.state = CONN_UNKNOWN; C.cur.reason[0] = '\0'; C.cur.since_ms = -1; C.cur.updated_ms = 0;
    pthread_mutex_unlock(&C.mu);
}
