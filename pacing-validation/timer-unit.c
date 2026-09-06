/* Actual nginx timer implementation; deterministic clock and callbacks. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include "ngx_event_timer.c"

volatile ngx_msec_t ngx_current_msec;
static ngx_usec_t now;
static unsigned clock_reads, checks, fired, rearm, cancel_other, release_event;
static ngx_event_t *other;
static ngx_log_t test_log;
static ngx_connection_t connection;
static unsigned order[32];

#define check(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)

ngx_usec_t ngx_monotonic_usec(void) { ++clock_reads; return now; }
void ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...) {}

static void handler(ngx_event_t *ev) {
    check(!ev->timer_set && !ev->timer_precise && ev->timedout);
    if (fired < 32) order[fired] = ev->index;
    ++fired;
    if (cancel_other) {
        cancel_other = 0;
        ngx_del_timer(other);
    }
    if (release_event) {
        release_event = 0;
        free(ev);
        return;
    }
    if (rearm) {
        rearm = 0;
        now += 10;
        ngx_event_add_precise_timer(ev, 1);
    }
}

static void init_event(ngx_event_t *ev, unsigned id) {
    memset(ev, 0, sizeof(*ev));
    ev->log = &test_log; ev->data = &connection;
    ev->handler = handler; ev->index = id;
}

static void reset(void) {
    ngx_current_msec = 1000; now = 1000000;
    fired = rearm = cancel_other = release_event = clock_reads = 0;
    ngx_event_timer_init(&test_log);
}

int main(void) {
    ngx_event_t a, b, c, *heap, *many;
    unsigned i, reads;
    reset(); init_event(&a, 1); init_event(&b, 2); init_event(&c, 3);
    check(ngx_event_find_timer() == NGX_TIMER_INFINITE);
    check(ngx_event_find_precise_timer() == NGX_PRECISE_TIMER_INFINITE);
    check(ngx_event_timer_timeout(NGX_TIMER_INFINITE) == NGX_TIMER_INFINITE);
    ngx_event_expire_timers();
    check(clock_reads == 0);
    check(ngx_event_no_timers_left() == NGX_OK);
    ngx_event_add_precise_timer(&a, 150);
    check(a.timer_set && a.timer_precise);
    check(ngx_event_find_timer() == NGX_TIMER_INFINITE);
    check(ngx_event_find_precise_timer() == 150);
    check(ngx_event_timer_timeout(NGX_TIMER_INFINITE) == 1);
    check(ngx_event_timer_timeout(0) == 0);
    check(ngx_event_no_timers_left() == NGX_AGAIN);
    a.cancelable = 1;
    check(ngx_event_no_timers_left() == NGX_OK);
    a.cancelable = 0;
    now += 149; ngx_event_expire_timers(); check(fired == 0);
    check(ngx_event_find_precise_timer() == 1);
    now += 1; ngx_event_expire_timers(); check(fired == 1);
    check(!a.timer_set && !a.timer_precise);

    reset();
    ngx_event_add_precise_timer(&a, 1001);
    check(ngx_event_timer_timeout(20) == 2);
    check(ngx_event_timer_timeout(1) == 1);
    now += 1; check(ngx_event_timer_timeout(20) == 1);
    now += 1000; check(ngx_event_timer_timeout(20) == 0);
    ngx_del_timer(&a);
    check(ngx_event_no_timers_left() == NGX_OK);

    reset();
    ngx_add_timer(&a, 500);
    ngx_event_add_precise_timer(&a, 150);
    check(ngx_event_find_timer() == NGX_TIMER_INFINITE);
    check(ngx_event_find_precise_timer() == 150);
    ngx_event_add_precise_timer(&a, 40);
    check(ngx_event_find_precise_timer() == 40);
    ngx_add_timer(&a, 25);
    check(!a.timer_precise && a.timer_set);
    check(ngx_event_find_precise_timer() == NGX_PRECISE_TIMER_INFINITE);
    check(ngx_event_find_timer() == 25);
    ngx_add_timer(&a, 30);
    check(ngx_event_find_timer() == 25);
    ngx_del_timer(&a);
    ngx_event_add_precise_timer(&a, 40);
    ngx_del_timer(&a);
    check(ngx_event_find_precise_timer() == NGX_PRECISE_TIMER_INFINITE);

    reset();
    ngx_event_add_precise_timer(&a, 50);
    ngx_event_add_precise_timer(&b, 20);
    ngx_event_add_precise_timer(&c, 50);
    now += 20; ngx_event_expire_timers(); check(fired == 1 && order[0] == 2);
    now += 30; ngx_event_expire_timers(); check(fired == 3);
    check((order[1] == 1 && order[2] == 3) ||
          (order[1] == 3 && order[2] == 1));

    reset(); now = (ngx_usec_t) -21;
    ngx_event_add_precise_timer(&a, 30);
    ngx_event_add_precise_timer(&b, 10);
    check(ngx_event_find_precise_timer() == 10);
    now += 10; ngx_event_expire_timers(); check(fired == 1 && order[0] == 2);
    now += 19; ngx_event_expire_timers(); check(fired == 1);
    now += 1; ngx_event_expire_timers(); check(fired == 2 && order[1] == 1);

    reset();
    ngx_event_add_precise_timer(&a, 10);
    ngx_event_add_precise_timer(&b, 20);
    other = &b; cancel_other = 1;
    now += 30; ngx_event_expire_timers();
    check(fired == 1 && !b.timer_set);
    check(ngx_event_no_timers_left() == NGX_OK);

    reset();
    ngx_event_add_precise_timer(&a, 10);
    rearm = 1; now += 10;
    ngx_event_expire_timers();
    check(fired == 1 && a.timer_set);
    check(ngx_event_find_precise_timer() == 1);
    now += 1; ngx_event_expire_timers(); check(fired == 2);

    reset();
    heap = calloc(1, sizeof(*heap)); check(heap != NULL);
    init_event(heap, 4); release_event = 1;
    ngx_event_add_precise_timer(heap, 1);
    now += 1; ngx_event_expire_timers(); check(fired == 1);

    reset();
    ngx_add_timer(&a, 2); a.cancelable = 1;
    ngx_event_add_precise_timer(&b, 20);
    check(ngx_event_no_timers_left() == NGX_AGAIN);
    b.cancelable = 1;
    check(ngx_event_no_timers_left() == NGX_OK);
    ngx_del_timer(&a); ngx_del_timer(&b);
    a.cancelable = b.cancelable = 0;

    reset();
    ngx_add_timer(&a, 1);
    ngx_event_add_precise_timer(&b, 2000);
    ngx_current_msec += 1;
    ngx_event_expire_timers(); check(fired == 1 && b.timer_set);
    now += 2000; ngx_event_expire_timers(); check(fired == 2);
    reads = clock_reads;
    ngx_event_expire_timers(); check(clock_reads == reads);

    reset();
    many = calloc(10000, sizeof(*many)); check(many != NULL);
    for (i = 0; i < 10000; ++i) {
        init_event(&many[i], i);
        ngx_event_add_precise_timer(&many[i], 1 + (i * 7919u) % 1000);
    }
    for (i = 0; i < 10000; i += 3) ngx_del_timer(&many[i]);
    now += 1000; ngx_event_expire_timers();
    check(fired == 6666);
    check(ngx_event_no_timers_left() == NGX_OK);
    free(many);
    printf("PASS: %u checks, including 10,000 timers, wrap, cancellation, "
           "unit transitions, lifetime and expiry fairness\n", checks);
    return 0;
}
