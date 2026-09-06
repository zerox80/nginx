# QUIC pacing review materials

These are companion materials for nginx commit 64aafe01584a2e98b630cf7bc9eca53257a6c5ae, based on nginx 1.31.5. This branch is separate from the two-commit nginx pull request. It adds no code to that PR.

BENCHMARKS.md and measurement-summary.json retain both benefits and regressions. The linked historical reports describe the completed experiments; statements there about old branch/PR status are historical, not current instructions.

The C harnesses include actual nginx source with fake clock/I/O boundaries. Build them against the matching configured nginx source. They do not replace network protocol tests. h3_pacing_bulk.t is the preserved companion fixture for nginx/nginx-tests at 9f1db4a63b14d2898652493735987d30ea61736f; copy it there to run it with the existing test framework.

Final protocol checks used h3_congestion_ack.t, h3_pacing_bulk.t, h3_keepalive.t and quic_migration.t with GSO off/on, in an isolated loopback namespace. Each run passed 4 files / 41 planned assertions, retaining 2 upstream TODO/skips. The normal nginx binary was used for protocol tests. ASan/UBSan were used for the timer/pacer C harnesses.

No throughput test depends on a timing pass/fail threshold. bench.py is retained as the controlled namespace measurement driver; running a fresh benchmark campaign is not required to review the PR.

AI assistance: OpenAI Codex assisted with this implementation and validation.

Example after configuring the matching nginx checkout with HTTP/3:

    OPENSSL_PREFIX=/path/to/openssl bash run-unit.sh /path/to/nginx objs

For protocol tests, copy h3_pacing_bulk.t into the nginx-tests checkout, then run:

    TEST_NGINX_BINARY=/path/to/nginx/objs/nginx \
    TEST_NGINX_GLOBALS_HTTP="quic_gso on;" \
    prove -v h3_congestion_ack.t h3_pacing_bulk.t h3_keepalive.t quic_migration.t

Repeat with quic_gso off. Run the test framework under an account with permission to read its temporary files; the final root namespace run used TEST_NGINX_GLOBALS="user root;".
