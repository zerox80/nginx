/* Deterministic output-path tests: real nginx output code and timer tree,
 * fake clock, serializer, crypto, ACK generator and sendmsg boundary.
 * Protocol interoperability is covered separately by nginx-tests. */
#include <ngx_config.h>
#include <assert.h>
#include <ngx_event_quic_connection.h>
#include "ngx_event_quic_output.c"
#include "ngx_event_timer.c"

volatile ngx_msec_t ngx_current_msec;
static ngx_usec_t now, send_cost;
ngx_usec_t ngx_monotonic_usec(void) { return now; }
static ngx_connection_t conn;
static ngx_quic_connection_t qc;
static ngx_quic_socket_t sock;
static ngx_quic_path_t path;
static ngx_quic_client_id_t cid;
static ngx_quic_conf_t conf;
static ngx_event_t read_event;
static ngx_log_t log_data;
static ngx_quic_frame_t frames[512];
static unsigned used, sends, segments, max_segments, checks;
static size_t wire_bytes;
static ssize_t send_result;
static ngx_msec_t ack_timer;

#define check(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)

void ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...) {}
void ngx_quic_log_frame(ngx_log_t *log, ngx_quic_frame_t *f, ngx_uint_t tx) {}
ngx_uint_t ngx_quic_keys_available(ngx_quic_keys_t *keys, ngx_uint_t level,
    ngx_uint_t write) { return 1; }
size_t ngx_quic_payload_size(ngx_quic_header_t *pkt, size_t n) {
    return n > 32 ? n - 32 : 0;
}
ssize_t ngx_quic_create_frame(u_char *p, ngx_quic_frame_t *f) {
    if (p) memset(p, 0, f->len);
    return f->len;
}
ngx_int_t ngx_quic_split_frame(ngx_connection_t *c, ngx_quic_frame_t *f,
    size_t len) { return NGX_DECLINED; }
ngx_int_t ngx_quic_encrypt(ngx_quic_header_t *pkt, ngx_str_t *res) {
    res->len = pkt->payload.len + 32;
    memset(res->data, 0, res->len);
    return NGX_OK;
}
void ngx_quic_free_frame(ngx_connection_t *c, ngx_quic_frame_t *f) {}
void ngx_quic_congestion_idle(ngx_connection_t *c, ngx_uint_t idle) {
    qc.congestion.idle = idle;
}
void ngx_quic_set_lost_timer(ngx_connection_t *c) {}
ngx_int_t ngx_quic_generate_ack(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx) {
    if (ack_timer && !qc.push.timer_set) ngx_add_timer(&qc.push, ack_timer);
    return NGX_OK;
}
size_t ngx_set_srcaddr_cmsg(struct cmsghdr *cmsg, struct sockaddr *addr) {
    abort();
}
ssize_t ngx_sendmsg(ngx_connection_t *c, struct msghdr *msg, int flags) {
    struct cmsghdr *cm;
    size_t n = msg->msg_iov[0].iov_len;
    unsigned seg = 1;
    if (send_result) return send_result;
    for (cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == SOL_UDP && cm->cmsg_type == UDP_SEGMENT) {
            size_t size = *(uint16_t *) CMSG_DATA(cm);
            check(n % size == 0);
            seg = n / size;
        }
    }
    now += send_cost;
    ++sends;
    segments += seg;
    if (seg > max_segments) max_segments = seg;
    wire_bytes += n;
    return n;
}

static void setup(int gso) {
    unsigned i;
    memset(&qc, 0, sizeof(qc)); memset(&conn, 0, sizeof(conn));
    memset(&path, 0, sizeof(path)); memset(&conf, 0, sizeof(conf));
    memset(&read_event, 0, sizeof(read_event));
    used = sends = segments = max_segments = 0;
    wire_bytes = 0; send_result = 0; ack_timer = 0;
    ngx_current_msec = 1000; now = 1000000; send_cost = 0;
    ngx_event_timer_init(&log_data);
    conn.udp = &sock.udp; sock.quic = &qc; conn.log = &log_data;
    conn.read = &read_event; read_event.data = &conn;
    read_event.log = &log_data;
    qc.path = &path; path.mtu = 1200; path.validated = 1; path.cid = &cid;
    qc.conf = &conf; conf.gso_enabled = gso;
    qc.push.data = &conn; qc.push.log = &log_data;
    qc.congestion.window = 96000; qc.congestion.ssthresh = 96000;
    qc.avg_rtt = 100; qc.first_rtt = 900; qc.tp.max_idle_timeout = 30000;
    for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; ++i) {
        ngx_queue_init(&qc.send_ctx[i].frames);
        ngx_queue_init(&qc.send_ctx[i].sending);
        ngx_queue_init(&qc.send_ctx[i].sent);
        qc.send_ctx[i].level = i == 0 ? 0 : i + 1;
        qc.send_ctx[i].largest_ack = NGX_QUIC_UNSET_PN;
    }
}
static void queue_frame(unsigned ctx, int ack, int head) {
    ngx_quic_frame_t *f = &frames[used++];
    assert(used <= 512);
    memset(f, 0, sizeof(*f));
    f->len = ack ? 20 : 1000; f->need_ack = !ack;
    f->type = ack ? NGX_QUIC_FT_ACK : NGX_QUIC_FT_STREAM;
    if (head) {
        ngx_queue_insert_head(&qc.send_ctx[ctx].frames, &f->queue);
    } else {
        ngx_queue_insert_tail(&qc.send_ctx[ctx].frames, &f->queue);
    }
}
static void queue_data(unsigned count) {
    while (count--) queue_frame(2, 0, 0);
}
static void bucket(size_t budget, size_t rate) {
    qc.pacing.budget = budget; qc.pacing.rate = rate;
    qc.pacing.last = now;
}
static void exercise_output(int gso) {
    size_t credit, flight, before;
    setup(gso); qc.avg_rtt = 20;
    bucket(7200, 6000); queue_data(200); credit = qc.pacing.budget;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(qc.congestion.in_flight <= credit);
    check(qc.congestion.in_flight + qc.pacing.budget == credit);
    check(wire_bytes == qc.congestion.in_flight);
    check(qc.push.timer_set && qc.push.timer_precise && qc.push.timer.key > now);
    check(!qc.congestion.idle);
    check(max_segments <= NGX_QUIC_PACING_MAX_BURST);
    check(gso ? max_segments > 1 : max_segments == 1);
    before = wire_bytes;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(wire_bytes == before); /* repeated posts do not mint credit */
    flight = qc.congestion.in_flight;
    queue_frame(2, 1, 1);
    check(ngx_quic_output(&conn) == NGX_OK);
    check(wire_bytes > before); /* ACK passes exhausted pacing budget */
    check(qc.congestion.in_flight == flight);
    before = wire_bytes;
    ngx_current_msec += 1; now += (ngx_usec_t) 1 * 1000;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(wire_bytes > before);

    setup(gso); qc.avg_rtt = 20; bucket(7200, 6000); queue_data(100);
    send_result = NGX_AGAIN;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(qc.pacing.budget == 7200 && qc.congestion.in_flight == 0);
    check(qc.send_ctx[2].pnum == 0);
    check(ngx_queue_empty(&qc.send_ctx[2].sending));
    check(qc.push.timer_set && qc.push.timer.key == ngx_current_msec + 10);
    send_result = 0;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(qc.congestion.in_flight > 0);
}
int main(void) {
    size_t first, budget;
    unsigned i;
    exercise_output(0); exercise_output(1);

    setup(1); qc.avg_rtt = 1; qc.congestion.window = 1000000;
    bucket(500000, 1000000); queue_data(400);
    check(ngx_quic_output(&conn) == NGX_OK);
    check(max_segments == 10 && sends == 1);
    check(qc.congestion.in_flight <= 10 * path.mtu);
    first = wire_bytes;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(wire_bytes == first); /* a second syscall cannot bypass the cap */

    setup(0); qc.avg_rtt = 1; qc.congestion.window = 1000000;
    bucket(500000, 1000000); queue_data(400);
    check(ngx_quic_output(&conn) == NGX_OK);
    check(wire_bytes <= NGX_QUIC_PACING_MAX_BURST * path.mtu);
    first = wire_bytes;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(wire_bytes == first);

    /* Coalesced Initial + Handshake + Application use the same credit.
     * GSO is configured, but the Initial queue forces datagram fallback. */
    setup(1); bucket(1200, 1200);
    for (i = 0; i < 3; ++i) {
        queue_frame(i, 0, 0); frames[used - 1].len = 100;
    }
    check(ngx_quic_output(&conn) == NGX_OK);
    check(sends == 1 && wire_bytes == 1200);
    check(qc.congestion.in_flight == 1200 && qc.pacing.budget == 0);
    for (i = 0; i < 3; ++i) check(qc.send_ctx[i].pnum == 1);

    /* Pacing cannot override Initial padding or anti-amplification. */
    setup(1); bucket(2400, 1200); path.validated = 0; path.received = 399;
    queue_frame(0, 0, 0);
    check(ngx_quic_output(&conn) == NGX_OK);
    check(sends == 0 && qc.pacing.budget == 2400);
    check(qc.send_ctx[0].pnum == 0);
    path.received = 400;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(wire_bytes == 1200 && qc.congestion.in_flight == 1200);

    setup(0); bucket(0, 1200); queue_frame(2, 1, 1);
    qc.congestion.in_flight = qc.congestion.window;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(sends == 1 && qc.pacing.budget == 0);
    check(!qc.push.timer_set);

    setup(0); bucket(0, 1200); /* no queued data, no pacing wakeups */
    check(ngx_quic_output(&conn) == NGX_OK);
    check(!qc.push.timer_set);

    setup(0); bucket(0, 1200); queue_data(1); qc.closing = 1;
    ngx_add_timer(&qc.push, 50);
    check(ngx_quic_output(&conn) == NGX_OK);
    check(!qc.push.timer_set);

    setup(0); bucket(0, 1200); queue_data(1); ack_timer = 25;
    ngx_add_timer(&qc.push, 200);
    check(ngx_quic_output(&conn) == NGX_OK);
    check(qc.push.timer_precise && qc.push.timer.key == now + 1000);

    setup(0); qc.avg_rtt = 10000; bucket(0, 1); queue_data(1); ack_timer = 2;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(qc.push.timer.key == ngx_current_msec + 2); /* earlier ACK wins */

    setup(0); ngx_quic_init_rtt(&qc); qc.congestion.window = 12000;
    qc.congestion.ssthresh = (size_t) -1;
    ngx_quic_pacing_update(&conn); check(qc.pacing.budget == 12000);
    check(qc.pacing.rate > 0); qc.pacing.budget = 0;
    ngx_current_msec += 10000000; now += (ngx_usec_t) 10000000 * 1000;
    ngx_quic_pacing_update(&conn); check(qc.pacing.budget == 12000);

    setup(0); bucket(0, 10); ngx_current_msec += 10; now += (ngx_usec_t) 10 * 1000;
    ngx_quic_pacing_update(&conn); check(qc.pacing.budget == 100);
    first = qc.pacing.budget;
    ngx_current_msec -= 1; now -= 1000; ngx_quic_pacing_update(&conn);
    check(qc.pacing.budget == first); /* backwards clock */
    qc.pacing.last = (ngx_usec_t) -5000; now = 5000; ngx_current_msec = 5;
    ngx_quic_pacing_update(&conn); check(qc.pacing.budget > first);

    setup(0); bucket(50000, 10000); qc.congestion.window = 2400;
    ngx_quic_pacing_update(&conn); check(qc.pacing.budget <= 2400);
    path.mtu = 65000; ngx_quic_pacing_update(&conn);
    ngx_current_msec += 1000000; now += (ngx_usec_t) 1000000 * 1000; ngx_quic_pacing_update(&conn);
    check(qc.pacing.budget >= path.mtu); /* MTU growth cannot starve output */

    setup(0); qc.avg_rtt = 0; qc.congestion.window = (size_t) -1;
    qc.congestion.ssthresh = (size_t) -1;
    for (i = 0; i < 4; ++i) {
        ngx_quic_pacing_update(&conn); check(qc.pacing.rate > 0);
        budget = qc.pacing.budget;
        ngx_current_msec += 100000000; now += (ngx_usec_t) 100000000 * 1000; qc.pacing.budget = 0;
        ngx_quic_pacing_update(&conn); check(qc.pacing.budget == budget);
    }

    /* Sub-millisecond credit works even when the cached millisecond is fixed. */
    for (i = 0; i < 2; ++i) {
        setup(i); qc.avg_rtt = 4; bucket(0, 30000); queue_data(400);
        for (unsigned step = 0; step < 10; ++step) {
            first = wire_bytes;
            now += 100;
            check(ngx_quic_output(&conn) == NGX_OK);
            check(wire_bytes - first <= 10 * path.mtu);
        }
        check(ngx_current_msec == 1000);
        check(wire_bytes > 2 * 10 * path.mtu);
        check(wire_bytes <= 30000);
    }

    /* Repeated tiny updates retain fractions instead of creating/dropping bytes. */
    setup(0); qc.avg_rtt = 96000; bucket(0, 1);
    for (i = 0; i < 1000; ++i) {
        ++now; ngx_quic_pacing_update(&conn);
        check(qc.pacing.budget == (i + 1) / 1000);
        check(qc.pacing.fraction == (i + 1) % 1000);
    }

    setup(0); qc.avg_rtt = 20; bucket(0, 6000); queue_data(1);
    now += 900; qc.pacing.last = now; ack_timer = 1;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(qc.push.timer_set && !qc.push.timer_precise);
    check(qc.push.timer.key == ngx_current_msec + 1);

    /* A long send/encryption interval must not start the deficit wait anew. */
    setup(0); bucket(1200, 1200); queue_data(10); send_cost = 2000;
    check(ngx_quic_output(&conn) == NGX_OK);
    check(qc.push.timer_precise && qc.push.timer.key == now + 1);

    /* An idle interval beyond the 32-bit microsecond wrap still fills safely. */
    setup(0); bucket(0, 1); qc.avg_rtt = 96000;
    now += (ngx_usec_t) 3600 * 1000000;
    ngx_quic_pacing_update(&conn);
    check(qc.pacing.budget == 2400 && qc.pacing.fraction == 0);

    setup(0); qc.avg_rtt = 96000; bucket(2400, 1);
    qc.pacing.fraction = 999; ++now;
    ngx_quic_pacing_update(&conn);
    check(qc.pacing.budget == 2400 && qc.pacing.fraction == 0);

    /* Clock regression must not move last backwards and mint the interval twice. */
    setup(0); bucket(0, 1200);
    now -= 50; ngx_quic_pacing_update(&conn);
    check(qc.pacing.last == 1000000 && qc.pacing.budget == 0);
    now += 51; ngx_quic_pacing_update(&conn);
    check(qc.pacing.budget == 1 && qc.pacing.fraction == 200);

    printf("PASS: %u deterministic checks (datagrams, GSO, ACK, rollback, timers, arithmetic)\n", checks);
    return 0;
}
