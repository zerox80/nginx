# QUIC pacing: microsecond scheduler prototype

Generated: 2026-09-06T16:17:36.382486+00:00

## Result and scope

A working local prototype integrates microsecond deadlines with nginx's event
loop and the shared QUIC datagram/GSO budget. It removes the previous mandatory
millisecond refill step on the supported Linux backend. It remains an
experiment for review and controlled host measurements, not an upstream-ready
production patch or a proven fix for issue #1498.

This report is the WSL validation snapshot completed before host testing.
Windows and the same Codex task remain the working environment. The user
subsequently supplied a Hetzner instance for SSH testing; its results are
recorded separately. No push or PR was made.

The previous prototype remains intact at /home/ruji/src/nginx on
codex/quic-pacing. This iteration is /home/ruji/src/nginx-usec on
codex/quic-pacing-usec. Source changes remain uncommitted.

Base and freshly fetched upstream master:
231a60ee3e90a43b829b9ca0a3013a8359b98d7e.
The fetch on 2026-09-06 at 15:52 UTC found no newer master commit.
nginx-tests remains pinned at 9f1db4a63b14d2898652493735987d30ea61736f,
with the previously added h3_pacing_bulk.t fixture. The original Windows
checkout is unchanged.

## Implementation

Queued STREAM/CRYPTO frames pass through the shared QUIC output entry, then
normal coalesced datagrams or GSO segments, followed by successful-send
accounting. Congestion control limits bytes in flight; pacing separately
limits eligible ack-eliciting bytes. A single connection-wide ten-path-MTU
budget constrains both paths and successive syscalls. ACK-only packets and
existing bounded direct recovery/path-control sends retain their exemptions.

A separate microsecond timer rbtree reuses the event's existing timer node,
with a flag selecting its tree. Generic deletion and expiry clear membership
and the flag. Switching timer units removes the old node before insertion.
Precise updates have no 300 ms lazy tolerance. Ordinary timer keys and the
event backend function signature remain in milliseconds. Expiry takes one
clock snapshot per tree; later rearmed precise timers cannot monopolize that
pass. Graceful shutdown checks both trees.

A 64-bit CLOCK_MONOTONIC reading supplies precise time independently of the
cached millisecond clock and timer_resolution. Only timer keys are truncated
on 32-bit builds; signed modular comparisons handle wrap. Relative precise
timers must remain within half the key range. The pacer's largest deficit
wait satisfies that contract. Its 64-bit timestamp avoids a 35-minute idle-time
ambiguity from a 32-bit microsecond elapsed counter. Systems lacking
CLOCK_MONOTONIC use the cached millisecond clock as a coarse fallback.

Every backend receives a rounded-up millisecond timeout for the earliest
precise deadline. Linux additionally uses configure-detected epoll_pwait2
while a precise timer exists. ENOSYS or EPERM disables that optional call once
per worker and falls back to epoll_wait. An empty precise tree adds neither
a clock read nor another wait syscall. kqueue's timespec conversion was adapted,
including its existing Darwin workaround, but was not compiled or run here.
Other backends retain coarse waits.

Pacing rates remain integer bytes per millisecond. Credit uses elapsed
microseconds and fractional bytes in thousandths. Saturation is checked before
multiplying elapsed time by rate. The deficit is at most ten legal UDP payload
sizes times 1000 and fits 32-bit size_t. Rate increases are not retroactive;
a backwards clock cannot move the refill timestamp backwards. Construction
time is deducted from the next deficit wait. Earlier ACK deadlines are
compared in the same monotonic clock domain and preserved.

No BBR, SO_TXTIME, ETF, configuration directive, floating-point arithmetic,
per-packet allocation, per-connection timer fd, new congestion control or
RTT-estimator change was introduced.

## Modified production files

- auto/os/linux
- src/core/ngx_times.c
- src/core/ngx_times.h
- src/event/modules/ngx_epoll_module.c
- src/event/modules/ngx_kqueue_module.c
- src/event/ngx_event.c
- src/event/ngx_event.h
- src/event/ngx_event_timer.c
- src/event/ngx_event_timer.h
- src/event/quic/ngx_event_quic.c
- src/event/quic/ngx_event_quic_connection.h
- src/event/quic/ngx_event_quic_migration.c
- src/event/quic/ngx_event_quic_output.c

Diff summary: 13 files changed, 402 insertions(+), 17 deletions(-)

The complete patch is quic-pacing-usec.patch. The two numbered patches apply
sequentially to the pinned clean base: timing foundation, then QUIC pacing.
quic-pacing-ms.patch preserves the earlier experiment; do not combine it with
the new combined or numbered patches.

## Builds

GCC 13.3 and Clang 18.1.3 builds include HTTP SSL, HTTP/2, HTTP/3 and debug
against the same static OpenSSL 3.5.8 used previously. nginx -V and nginx -t
with GSO off/on pass for both normal builds. These build logs contain no
compiler warning or error caused by the patch.

Separate Clang AddressSanitizer-only and combined ASan/UBSan builds completed.
They omit the optional Perl module to avoid mixing its configured GCC ASan
runtime with Clang's runtime. Their test outcomes below are not blanket
sanitizer passes.

A real 32-bit GCC nginx core build (HTTP disabled) completed using multilib.
The timer code also passed its 32-bit ASan/UBSan harness. This is not a 32-bit
HTTP/3/OpenSSL build. GCC/Clang separately compiled QUIC output with GSO
excluded, epoll without epoll_pwait2, and the clock without CLOCK_MONOTONIC.
Source and binary hashes are in evidence/build-source-manifest.json.

## Focused tests

- Timer harness: 6,729 checks under Clang ASan/UBSan on 64-bit and GCC ASan/UBSan
  on real 32-bit. Includes 10,000 queued timers, cancellation, 6,666 resulting
  callback-state checks, duplicate deadlines, wrap, timer-unit transitions,
  callback cancellation/free/rearm, lazy ordinary updates, shutdown and
  independent cached/fine clocks. The total includes repeated checks.
- QUIC output harness: 2,152 checks under Clang ASan/UBSan, including 2,000
  fractional-credit assertions. Executes actual output and timer code with
  fake time/I/O/serialization. Covers datagrams, GSO, coalescing, ACK exemption,
  EAGAIN and packet-number rollback, burst bounds, idle/close, zero RTT, large
  arithmetic, MTU/cwnd changes, sub-ms progress, earlier ACK deadlines,
  construction time, clock regression and long idle.
- Actual nginx epoll backend: ENOSYS/EPERM fallback, empty-tree ordinary wait,
  and ready pipe I/O while a precise timer is pending pass. Timed runs check
  that zero-timeout iterations process due callbacks, without a pass/fail
  latency or throughput threshold.
- Bulk/multiplexing fixture with timer_resolution 10ms: 14 assertions pass for
  each GSO setting. Ordinary RTT/ACK clock precision is unchanged.

Harness simplifications are documented in their headers. Protocol
interoperability is tested separately.

## Integration results

| Run | GSO | Files / assertions per run | Result |
| --- | --- | --- | --- |
| GCC, normal | off and on | 25 / 358 | PASS both |
| Clang, normal | off and on | 25 / 358 | PASS both |
| GCC, forced runtime ENOSYS | off and on | 25 / 358 | PASS both |
| Clang, ASan-only + leak detection | off and on | 25 / 358 | FAIL: same 2 keepalive checks each |
| GCC, complete suite | off | 498 / 6,785 | FAIL: 3 environment-dependent tunnel checks |

The complete run includes all 497 official files plus the bulk fixture, with
44 whole-file skips, 74 individual skip lines and upstream TODO cases. Its
only failing file is tunnel_next_upstream.t.

### Full-suite environment finding

The isolated namespace initially had loopback only. A test expects TCP connect
to 240.0.0.1 to time out; without a route it immediately returned ENETUNREACH.
Tests 2, 5 and 8 failed identically on stock and patched nginx.

A route to an owned dummy interface inside the isolated namespace provides
the intended silently dropped SYN path. All 10 assertions then pass on stock
and patch. Neither source nor assertions were changed. The entire 498-file
suite was not rerun after correcting that setup: its raw status remains FAIL,
with the successful focused reruns recorded separately.

### Sanitizer findings

ASan-only runs with leak detection and no suppressions pass the other 24 QUIC
files. h3_keepalive.t checks 16/17 fail during reload: LeakSanitizer reports
225,280 bytes in three allocations. A freshly built stock ASan-only binary
reproduces the same amount and allocation sites with GSO off/on: connection
and read/write event arrays allocated in ngx_event_process_init. All protocol
checks in that fixture pass. Matched logs include allocation stacks.

Strict combined ASan/UBSan h3_pacing_bulk.t aborts at the existing unaligned
access in ngx_event_quic_transport.c:194 before bulk transfer. The pinned stock
combined-sanitizer binary fails at the identical location. This is not a clean
combined integration run.

Timer and pacing harnesses themselves pass combined ASan/UBSan with leak
detection and no suppressions. Earlier stock findings are in the preserved
baseline report; unrelated fixes are outside this patch.

## WSL scheduler observations

The probe uses actual nginx timer/epoll code with 1/32/1,024 timers, requested
intervals 100/250/500/1,000 us, and both wait paths. Each sample is approximately
0.5 seconds. These are WSL observations, not stable host-capacity estimates or
packet-spacing measurements. Builds/tests may run concurrently.
The 100-us subset is:

| Wait path | Timers | Late p50 us | Late p99 us | Process CPU / elapsed | Zero-timeout calls | Empty zero-timeout iterations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| epoll_pwait2 | 1 | 69 | 347 | 5.0% | 0 | 0 |
| epoll_wait | 1 | 999 | 1744 | 1.6% | 0 | 0 |
| epoll_pwait2 | 32 | 77 | 301 | 9.1% | 0 | 0 |
| epoll_wait | 32 | 1002 | 1302 | 2.3% | 0 | 0 |
| epoll_pwait2 | 1024 | 16 | 266 | 85.7% | 33484 | 0 |
| epoll_wait | 1024 | 9 | 1119 | 64.2% | 33955 | 0 |

Many timers can consume most of a core executing real callbacks. Zero-timeout
polls then accompany already-due work; all 24 samples record zero empty
zero-timeout iterations. Fine deadlines do not guarantee exact wake times or
acceptable CPU cost at every load.

The initial collector read the rbtree key after deletion, which nginx clears.
Its invalid output is retained as timer-probe-invalid-cleared-key.jsonl and
excluded from conclusions. The fixed collector saves deadlines independently.
timer-probe-final.jsonl is the reported run with the empty-loop check;
timer-probe.jsonl is an earlier valid run.

## WSL transfer smoke test

All 24 eight-MiB transfers succeeded: HTTP/2 and HTTP/3, stock/patched, GSO
off/on, added RTT 0/4/16 ms. Status, protocol and byte count were checked.
One repetition, no warmup, no packet/qlog diagnostics and possible concurrent
builds/tests make this unsuitable for performance conclusions. Exact bodies
are checked separately by the protocol fixture.

All raw H3 values follow, including both improvements and regressions:

| Variant | GSO | Added RTT ms | Mbit/s | Server CPU seconds |
| --- | --- | ---: | ---: | ---: |
| stock | off | 0 | 1071.5 | 0.06 |
| patched | off | 0 | 367.9 | 0.13 |
| stock | on | 0 | 758.4 | 0.05 |
| patched | on | 0 | 153.6 | 0.22 |
| stock | off | 4 | 91.0 | 0.05 |
| patched | off | 4 | 220.4 | 0.14 |
| patched | on | 4 | 625.5 | 0.05 |
| stock | on | 4 | 296.3 | 0.03 |
| stock | off | 16 | 29.5 | 0.07 |
| patched | off | 16 | 145.9 | 0.21 |
| patched | on | 16 | 207.3 | 0.10 |
| stock | on | 16 | 127.2 | 0.02 |

Several transfers exceed the earlier 96–118 Mbit/s ceiling. The frozen-cached-
millisecond harness independently demonstrates that the mandatory one-ms
refill step is gone. These observations do not prove a benefit on the physical
path or show that pacing always outperforms stock.

## Remaining work and next host

This event-core change needs maintainer review, including module/header
compatibility and timer API scope. Only Linux epoll was exercised at runtime.
kqueue and other systems need build/runtime testing. Coarse backends do not
gain fine wakeups just from the new clock.

RTT estimation stays in milliseconds. Wake jitter, ACK timing, useful GSO
batch size, CPU, many-connection fairness, PMTU, flow control and physical-path
burst/loss behavior need measured evaluation. GSO segments still form
microbursts, bounded by the shared byte budget.

A controlled Linux host experiment is now useful. A Hetzner instance can be
accessed from this same Windows/WSL task via SSH; no dualboot or task migration
is needed. See TIMING-BENCHMARK.md and HETZNER.md. Nothing is deployed here.

## Primary references

- [nginx development guide](https://nginx.org/en/docs/dev/development_guide.html)
- [Linux epoll API](https://man7.org/linux/man-pages/man2/epoll_wait.2.html)
- [Monotonic clock API](https://man7.org/linux/man-pages/man2/clock_gettime.2.html)
- [FreeBSD kevent](https://man.freebsd.org/cgi/man.cgi?query=kevent&sektion=2&format=html)
- [RFC 9002 section 7.7](https://www.rfc-editor.org/rfc/rfc9002.html#section-7.7)
- [nginx issue #1498](https://github.com/nginx/nginx/issues/1498)
- [QUIC Steps study](https://arxiv.org/html/2505.09222v1)
