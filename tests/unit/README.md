# HTTP/3 request-body unit tests

These tests compile the production HTTP/3 frame parser, request-body filter,
flood accounting, buffer management, and memory pools. They replace logging,
transport side effects, and the downstream body consumer; no running server,
certificate, or QUIC client is needed.

The test translation unit includes `ngx_http_v3_request.c` to access its static
body filter without changing the production API. Function sections and linker
garbage collection remove unrelated server code.

## Run

On Linux (including WSL), install a C compiler, GNU Make, and OpenSSL development
headers. From the repository root:

```sh
sh auto/configure --with-http_v3_module --without-http_rewrite_module \
    --without-http_gzip_module
make -C tests/unit check
```

The configure step generates NGINX's platform headers. An existing default
`objs/` configuration with HTTP/3 enabled also works.

To run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -C tests/unit clean
UBSAN_OPTIONS=halt_on_error=1 make -C tests/unit check CC=clang \
    CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
    LDFLAGS='-fsanitize=address,undefined'
```

Each case is checked with every possible two-buffer split and with one byte per
buffer, in buffered and unbuffered modes, with FIN either attached to the final
input buffer or supplied separately. Assertions cover forwarded body bytes,
Content-Length, body-size limits, HTTP/3 byte accounting, and truncated payloads.
