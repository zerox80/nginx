/*
 * Exercise the production body filter without making its static entry point
 * public.  The linker discards unrelated HTTP request handling functions.
 */

#include "../../src/http/v3/ngx_http_v3_request.c"


typedef struct {
    const char  *name;
    ngx_str_t    wire;
    off_t        max_body_size;
    off_t        content_length;
    ngx_int_t    status;
    ngx_str_t    body;
} test_case_t;


static test_case_t  test_cases[] = {
    {
        "ordinary fragmented DATA",
        ngx_string("\x00\x05" "hello"),
        5, -1, NGX_OK, ngx_string("hello")
    },
    {
        "unknown payload larger than the body limit",
        ngx_string("\x21\x07" "padding"),
        4, -1, NGX_OK, ngx_null_string
    },
    {
        "fragmented unknown frame before DATA",
        ngx_string("\x21\x07" "padding" "\x00\x03" "abc"),
        3, -1, NGX_OK, ngx_string("abc")
    },
    {
        "completed skip followed by a partial DATA length",
        ngx_string("\x21\x07" "padding" "\x00\x40\x03" "abc"),
        3, -1, NGX_OK, ngx_string("abc")
    },
    {
        "fragmented unknown frame between DATA frames",
        ngx_string("\x00\x03" "abc" "\x21\x07" "padding"
                   "\x00\x02" "de"),
        5, -1, NGX_OK, ngx_string("abcde")
    },
    {
        "trailing unknown frame",
        ngx_string("\x00\x05" "hello" "\x21\x07" "padding"),
        5, -1, NGX_OK, ngx_string("hello")
    },
    {
        "multiple unknown frames and fragmented varints",
        ngx_string("\x21\x00" "\x40\x21\x40\x07" "padding"
                   "\x21\x01" "x" "\x00\x03" "abc"),
        3, -1, NGX_OK, ngx_string("abc")
    },
    {
        "empty DATA and unknown frames",
        ngx_string("\x00\x00" "\x21\x00" "\x00\x03" "abc"),
        3, -1, NGX_OK, ngx_string("abc")
    },
    {
        "DATA limit after a fragmented unknown frame",
        ngx_string("\x21\x07" "padding" "\x00\x03" "abc"),
        2, -1, NGX_HTTP_REQUEST_ENTITY_TOO_LARGE, ngx_null_string
    },
    {
        "aggregate DATA limit across an unknown frame",
        ngx_string("\x00\x03" "abc" "\x21\x07" "padding"
                   "\x00\x03" "def"),
        5, -1, NGX_HTTP_REQUEST_ENTITY_TOO_LARGE, ngx_string("abc")
    },
    {
        "Content-Length counts only DATA",
        ngx_string("\x21\x07" "padding" "\x00\x03" "abc"),
        3, 3, NGX_OK, ngx_string("abc")
    },
    {
        "declared Content-Length is still enforced",
        ngx_string("\x21\x07" "padding" "\x00\x03" "abc"),
        10, 2, NGX_HTTP_BAD_REQUEST, ngx_null_string
    },
    {
        "disabled body limit still excludes unknown payload",
        ngx_string("\x21\x07" "padding" "\x00\x05" "hello"),
        0, -1, NGX_OK, ngx_string("hello")
    },
    {
        "truncated unknown payload is rejected",
        ngx_string("\x21\x07" "pad"),
        4, -1, NGX_HTTP_BAD_REQUEST, ngx_null_string
    },
    {
        "truncated DATA is rejected",
        ngx_string("\x00\x05" "abc"),
        5, -1, NGX_HTTP_BAD_REQUEST, ngx_string("abc")
    }
};


ngx_module_t  ngx_http_core_module;

static u_char      test_output[256];
static size_t      test_output_length;
static ngx_uint_t  test_last_buffers;
static ngx_uint_t  test_transport_errors;


static ngx_int_t
test_capture_body(ngx_http_request_t *r, ngx_chain_t *in)
{
    size_t        n;
    ngx_chain_t  *cl;

    for (cl = in; cl; cl = cl->next) {
        n = cl->buf->pos ? cl->buf->last - cl->buf->pos : 0;

        if (n > sizeof(test_output) - test_output_length) {
            return NGX_ERROR;
        }

        if (n) {
            ngx_memcpy(test_output + test_output_length, cl->buf->pos, n);
            test_output_length += n;
        }

        test_last_buffers += cl->buf->last_buf;
        cl->buf->pos = cl->buf->last;
    }

    return NGX_OK;
}


ngx_http_request_body_filter_pt  ngx_http_top_request_body_filter =
    test_capture_body;


/* Logging and transport side effects are the only replaced dependencies. */

void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{
}


ngx_int_t
ngx_quic_reset_stream(ngx_connection_t *c, ngx_uint_t err)
{
    test_transport_errors++;
    return NGX_OK;
}


void
ngx_quic_finalize_connection(ngx_connection_t *c, ngx_uint_t err,
    const char *reason)
{
    test_transport_errors++;
}


/* The filter uses this function's address solely as a buffer tag. */

ngx_int_t
ngx_http_read_client_request_body(ngx_http_request_t *r,
    ngx_http_client_body_handler_pt post_handler)
{
    abort();
}


static ngx_int_t
test_run(test_case_t *test, size_t split, ngx_uint_t separate_fin,
    ngx_uint_t unbuffered)
{
    void                      *loc_conf[1], *srv_conf[1];
    size_t                     offset, n;
    u_char                     input[256];
    ngx_int_t                  rc;
    ngx_buf_t                  buf;
    ngx_log_t                  log;
    ngx_pool_t                *pool;
    ngx_chain_t                in;
    ngx_connection_t           c;
    ngx_http_request_t         r;
    ngx_http_v3_parse_t        parse;
    ngx_http_v3_session_t      session;
    ngx_http_request_body_t    rb;
    ngx_http_core_loc_conf_t   clcf;
    ngx_http_core_srv_conf_t   cscf;

#define test_expect(condition)                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s: split=%zu, separate_fin=%lu, unbuffered=%lu: "    \
                "%s (status=%ld, received=%lld, output=%zu)\n",              \
                test->name, split, (unsigned long) separate_fin,             \
                (unsigned long) unbuffered, #condition, (long) rc,            \
                (long long) rb.received, test_output_length);                \
        ngx_destroy_pool(pool);                                              \
        return NGX_ERROR;                                                    \
    }

    ngx_memzero(&log, sizeof(log));
    ngx_memzero(&c, sizeof(c));
    ngx_memzero(&r, sizeof(r));
    ngx_memzero(&parse, sizeof(parse));
    ngx_memzero(&session, sizeof(session));
    ngx_memzero(&rb, sizeof(rb));
    ngx_memzero(&clcf, sizeof(clcf));
    ngx_memzero(&cscf, sizeof(cscf));

    pool = ngx_create_pool(4096, &log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    c.log = &log;
    c.data = &session;
    c.pool = pool;
    cscf.large_client_header_buffers.size = sizeof(input);
    clcf.client_max_body_size = test->max_body_size;
    loc_conf[0] = &clcf;
    srv_conf[0] = &cscf;

    r.connection = &c;
    r.pool = pool;
    r.loc_conf = loc_conf;
    r.srv_conf = srv_conf;
    r.v3_parse = &parse;
    r.request_body = &rb;
    r.request_body_no_buffering = unbuffered;
    r.headers_in.content_length_n = test->content_length;
    rb.rest = -1;

    test_output_length = 0;
    test_last_buffers = 0;
    test_transport_errors = 0;

    rc = NGX_OK;

    for (offset = 0; offset < test->wire.len; offset += n) {
        /* split == 0 supplies one byte per call; otherwise use two buffers. */
        n = split ? (offset ? test->wire.len - offset : split) : 1;
        test_expect(n <= sizeof(input));
        ngx_memcpy(input, test->wire.data + offset, n);

        ngx_memzero(&buf, sizeof(buf));
        buf.start = input;
        buf.pos = input;
        buf.last = input + n;
        buf.end = input + sizeof(input);
        buf.temporary = 1;
        buf.last_buf = !separate_fin && offset + n == test->wire.len;
        in.buf = &buf;
        in.next = NULL;

        rc = ngx_http_v3_request_body_filter(&r, &in);
        if (rc != NGX_OK) {
            break;
        }

        test_expect(buf.pos == buf.last);
    }

    if (rc == NGX_OK && separate_fin) {
        ngx_memzero(&buf, sizeof(buf));
        buf.start = input;
        buf.pos = input;
        buf.last = input;
        buf.end = input + sizeof(input);
        buf.last_buf = 1;
        in.buf = &buf;
        in.next = NULL;

        rc = ngx_http_v3_request_body_filter(&r, &in);
    }

    test_expect(rc == test->status);
    test_expect(test_transport_errors == 0);
    test_expect(test_output_length <= test->body.len);
    test_expect(test_output_length == 0
                || ngx_memcmp(test_output, test->body.data,
                              test_output_length) == 0);

    if (rc == NGX_OK) {
        test_expect(test_output_length == test->body.len);
        test_expect(rb.received == (off_t) test->body.len);
        test_expect(session.payload_bytes == (off_t) test->body.len);
        test_expect(session.total_bytes == (off_t) test->wire.len);
        test_expect(r.request_length == (off_t) test->wire.len);
        test_expect(r.headers_in.content_length_n == (off_t) test->body.len);
        test_expect(rb.rest == 0);
        test_expect(test_last_buffers == 1);

    } else {
        test_expect(test_last_buffers == 0);
    }

    ngx_destroy_pool(pool);
    return NGX_OK;

#undef test_expect
}


int
main(void)
{
    size_t      i, split;
    ngx_uint_t  separate_fin, unbuffered, failed, failures, runs;

    ngx_pagesize = (ngx_uint_t) getpagesize();
    ngx_http_core_module.ctx_index = 0;
    failures = 0;
    runs = 0;

    for (i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        failed = 0;

        for (split = 0; split <= test_cases[i].wire.len && !failed; split++) {
            for (separate_fin = 0; separate_fin < 2 && !failed; separate_fin++) {
                for (unbuffered = 0; unbuffered < 2; unbuffered++) {
                    runs++;

                    if (test_run(&test_cases[i], split, separate_fin, unbuffered)
                        != NGX_OK)
                    {
                        failed = 1;
                        break;
                    }
                }
            }
        }

        printf("%s: %s\n", failed ? "FAIL" : "PASS", test_cases[i].name);
        failures += failed;
    }

    printf("%lu runs, %lu failed cases\n", (unsigned long) runs,
           (unsigned long) failures);

    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
