/* Actual nginx epoll wait and timer code; owned epoll fd, no network traffic.
 * Wall-clock samples are observations, never pass/fail latency thresholds. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <assert.h>

static int injected_errno;
static unsigned long wait_calls, precise_calls, zero_waits, empty_zero_loops;
static int probe_epoll_wait(int fd, struct epoll_event *events, int n, int ms) {
    ++wait_calls;
    if (!ms) ++zero_waits;
    return epoll_wait(fd, events, n, ms);
}
static int probe_epoll_pwait2(int fd, struct epoll_event *events, int n,
    const struct timespec *ts, const sigset_t *mask)
{
    ++precise_calls;
    if (ts && !ts->tv_sec && !ts->tv_nsec) ++zero_waits;
    if (injected_errno) { errno = injected_errno; return -1; }
    return epoll_pwait2(fd, events, n, ts, mask);
}
static void probe_time_update(void) {
    ngx_current_msec = ngx_monotonic_usec() / 1000;
}
#define epoll_wait probe_epoll_wait
#define epoll_pwait2 probe_epoll_pwait2
#define ngx_time_update probe_time_update
#include "ngx_epoll_module.c"
#undef epoll_wait
#undef epoll_pwait2
#undef ngx_time_update
#include "ngx_event_timer.c"

sig_atomic_t ngx_event_timer_alarm;
ngx_queue_t ngx_posted_accept_events, ngx_posted_next_events, ngx_posted_events;
static ngx_log_t test_log;
static ngx_cycle_t test_cycle;
static ngx_connection_t connection;
static unsigned long callbacks, late_count;
static ngx_usec_t period, finish, *lateness;
static unsigned long sample_capacity = 4000000;
typedef struct {
    ngx_event_t event;
    ngx_usec_t deadline;
} sample_event;

void ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{
    if (level <= NGX_LOG_WARN) {
        fprintf(stderr, "unexpected nginx log: %s, errno %d\n", fmt, (int) err);
        abort();
    }
}
static void init_event(ngx_event_t *ev, ngx_event_handler_pt handler) {
    memset(ev, 0, sizeof(*ev));
    ev->log = &test_log; ev->data = &connection; ev->handler = handler;
}
static void expired(ngx_event_t *ev) {
    ngx_usec_t t = ngx_monotonic_usec();
    sample_event *sample = (sample_event *) ev;
    ngx_usec_int_t late = (ngx_usec_int_t) (t - sample->deadline);
    assert(late >= 0);
    ++callbacks;
    if (late_count < sample_capacity) lateness[late_count++] = late;
    if ((ngx_usec_int_t) (finish - t) > 0) {
        ngx_event_add_precise_timer(ev, period);
        sample->deadline = ev->timer.key;
    }
}
static int compare_usec(const void *a, const void *b) {
    ngx_usec_t x = *(const ngx_usec_t *) a, y = *(const ngx_usec_t *) b;
    return x < y ? -1 : x > y;
}
static uint64_t cpu_nsec(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0);
    return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void run_sample(unsigned events, unsigned usec, unsigned fallback) {
    sample_event *ev = calloc(events, sizeof(*ev));
    ngx_usec_t start, end;
    uint64_t cpu;
    unsigned i;
    unsigned long before, zeros;
    assert(ev != NULL);
    ngx_event_timer_init(&test_log);
    callbacks = late_count = wait_calls = precise_calls = zero_waits = 0;
    empty_zero_loops = 0;
    ngx_epoll_pwait2 = !fallback;
    period = usec;
    start = ngx_monotonic_usec(); finish = start + 500000;
    cpu = cpu_nsec();
    for (i = 0; i < events; ++i) {
        init_event(&ev[i].event, expired);
        ngx_event_add_precise_timer(&ev[i].event, period);
        ev[i].deadline = ev[i].event.timer.key;
    }
    while (ngx_event_no_timers_left() != NGX_OK) {
        ngx_msec_t ms = ngx_event_timer_timeout(NGX_TIMER_INFINITE);
        before = callbacks; zeros = zero_waits;
        assert(ngx_epoll_process_events(&test_cycle, ms, 0) == NGX_OK);
        ngx_event_expire_timers();
        if (zero_waits != zeros && callbacks == before) ++empty_zero_loops;
    }
    assert(empty_zero_loops == 0);
    end = ngx_monotonic_usec(); cpu = cpu_nsec() - cpu;
    qsort(lateness, late_count, sizeof(*lateness), compare_usec);
    printf("{\"backend\":\"%s\",\"timers\":%u,\"period_us\":%u,"
           "\"wall_us\":%lu,\"cpu_us\":%lu,\"callbacks\":%lu,"
           "\"samples\":%lu,\"epoll_wait_calls\":%lu,"
           "\"epoll_pwait2_calls\":%lu,\"zero_timeout_calls\":%lu,"
           "\"empty_zero_timeout_iterations\":%lu,"
           "\"lateness_p50_us\":%lu,\"lateness_p95_us\":%lu,"
           "\"lateness_p99_us\":%lu,\"lateness_max_us\":%lu}\n",
           fallback ? "epoll_wait" : "epoll_pwait2", events, usec,
           (unsigned long) (end - start), (unsigned long) (cpu / 1000),
           callbacks, late_count, wait_calls, precise_calls, zero_waits, empty_zero_loops,
           (unsigned long) lateness[late_count / 2],
           (unsigned long) lateness[late_count * 95 / 100],
           (unsigned long) lateness[late_count * 99 / 100],
           (unsigned long) lateness[late_count - 1]);
    fflush(stdout);
    free(ev);
}
static void noop(ngx_event_t *ev) {}
static void input_ready(ngx_event_t *ev) {
    char ch;
    ngx_connection_t *c = ev->data;
    assert(read(c->fd, &ch, 1) == 1 && ch == 'x');
    ++callbacks;
}
static void fallback_tests(void) {
    ngx_event_t ev, rev, wev;
    struct epoll_event ee;
    ngx_connection_t io;
    int fds[2], errors[] = { ENOSYS, EPERM };
    unsigned i;
    init_event(&ev, noop);
    for (i = 0; i < 2; ++i) {
        ngx_event_timer_init(&test_log);
        ngx_epoll_pwait2 = 1; injected_errno = errors[i];
        wait_calls = precise_calls = 0;
        ngx_event_add_precise_timer(&ev, 200);
        assert(ngx_epoll_process_events(&test_cycle,
               ngx_event_timer_timeout(NGX_TIMER_INFINITE), 0) == NGX_OK);
        assert(precise_calls == 1 && wait_calls == 1 && !ngx_epoll_pwait2);
        ngx_event_expire_timers();
        ngx_event_add_precise_timer(&ev, 200);
        assert(ngx_epoll_process_events(&test_cycle,
               ngx_event_timer_timeout(NGX_TIMER_INFINITE), 0) == NGX_OK);
        assert(precise_calls == 1 && wait_calls == 2);
        ngx_event_expire_timers();
    }
    injected_errno = 0; ngx_epoll_pwait2 = 1;
    ngx_event_timer_init(&test_log);
    wait_calls = precise_calls = 0;
    assert(ngx_epoll_process_events(&test_cycle, 0, 0) == NGX_OK);
    assert(wait_calls == 1 && precise_calls == 0);
    assert(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    memset(&io, 0, sizeof(io));
    init_event(&rev, input_ready); init_event(&wev, noop);
    io.fd = fds[0]; io.read = &rev; io.write = &wev;
    rev.data = &io; rev.active = 1;
    memset(&ee, 0, sizeof(ee)); ee.events = EPOLLIN; ee.data.ptr = &io;
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, io.fd, &ee) == 0);
    ngx_event_add_precise_timer(&ev, 1000000);
    assert(write(fds[1], "x", 1) == 1);
    callbacks = 0;
    assert(ngx_epoll_process_events(&test_cycle,
           ngx_event_timer_timeout(NGX_TIMER_INFINITE), 0) == NGX_OK);
    assert(callbacks == 1 && precise_calls == 1 && rev.ready);
    ngx_del_timer(&ev);
    close(fds[0]); close(fds[1]);
    fprintf(stderr, "PASS: actual epoll backend ENOSYS/EPERM fallback, "
            "empty-tree fast path and ready I/O with a precise timer\n");
}
int main(void) {
    unsigned i, j, k;
    unsigned counts[] = { 1, 32, 1024 }, periods[] = { 100, 250, 500, 1000 };
    test_cycle.log = &test_log;
    nevents = 8;
    event_list = calloc(nevents, sizeof(*event_list));
    lateness = calloc(sample_capacity, sizeof(*lateness));
    assert(event_list && lateness);
    ep = epoll_create1(EPOLL_CLOEXEC); assert(ep != -1);
    ngx_queue_init(&ngx_posted_accept_events);
    ngx_queue_init(&ngx_posted_next_events);
    ngx_queue_init(&ngx_posted_events);
    probe_time_update();
    fallback_tests();
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            for (k = 0; k < 2; ++k) run_sample(counts[i], periods[j], k);
    close(ep); free(event_list); free(lateness);
    return 0;
}
