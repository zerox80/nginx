# Retained QUIC pacing measurements

Source commit: 64aafe01584a2e98b630cf7bc9eca53257a6c5ae

No benchmark matrix was rerun to prepare the PR. This directory preserves the test sources and summaries; it contains no browser logs, packet captures, authentication data or generated TLS keys.

The full controlled and shallow-queue summaries, including ranges and CPU values, are in measurement-summary.json. That file also retains earlier public-path experiments; do not pool those experiments or substitute instrumented diagnostic throughput for quiet measurements. All warmup and repetition counts remain recorded.

The controlled pass comprised 224 transfers (168 measured, 56 warmups), including HTTP/2 controls. The shallow pass comprised 64 (48 measured, 16 warmups), also including controls. Every transfer completed with the requested protocol, HTTP 200 and expected byte count. Both protocols have outliers; medians of three are descriptive, not statistical guarantees.

Every table cell below is stock / pacer; server CPU is seconds per 32 MiB, at 10-ms granularity. Added RTT is not measured total RTT.

## Controlled Linux namespace pass

One worker, two-vCPU Ubuntu 26.04.1/Linux 7.0.0-30 KVM host, nginx 1.31.5, OpenSSL 3.5.8, TLS AES-128-GCM, 32-MiB static response, stream buffer 1 MiB, link MTU 1500, queue 1000 packets. Three measured transfers and one warmup per condition.

| Added RTT, ms | GSO off, Mbit/s | GSO on, Mbit/s | CPU off, s | CPU on, s |
|---:|---:|---:|---:|---:|
| 0 | 1192.2 / 1182.4 | 3574.3 / 2292.2 | 0.22 / 0.23 | 0.05 / 0.08 |
| 1 | 1057.1 / 772.6 | 657.0 / 1792.4 | 0.24 / 0.27 | 0.08 / 0.12 |
| 2 | 826.0 / 957.7 | 430.1 / 1806.5 | 0.22 / 0.26 | 0.09 / 0.12 |
| 4 | 654.0 / 930.6 | 231.6 / 1461.3 | 0.24 / 0.25 | 0.08 / 0.11 |
| 8 | 265.0 / 738.7 | 104.6 / 796.1 | 0.34 / 0.28 | 0.09 / 0.14 |
| 16 | 99.5 / 411.5 | 66.9 / 405.1 | 0.37 / 0.25 | 0.11 / 0.20 |
| 32 | 36.3 / 208.3 | 37.3 / 208.2 | 0.30 / 0.38 | 0.10 / 0.24 |

## Rate-limited shallow queue

Same setup, 250 Mbit/s and 64-packet queue.

| Added RTT, ms | GSO off, Mbit/s | GSO on, Mbit/s | CPU off, s | CPU on, s |
|---:|---:|---:|---:|---:|
| 4 | 207.2 / 201.2 | 197.6 / 196.4 | 0.44 / 0.47 | 0.21 / 0.50 |
| 16 | 42.3 / 65.2 | 82.5 / 63.8 | 0.62 / 0.93 | 0.18 / 0.90 |

## Additional evidence

- LOW-RTT-REPORT.md: the focused RTT-resolution/burst diagnosis and its limitations. Larger burst variants remain diagnostics.
- NATIVE-CLIENT-REPORT.md: the separate quiet native/WSL comparison, ranges, CPU, runtime matching and confounders.
- TIMING-REPORT.md: historical WSL build/protocol/sanitizer evidence. Its old uncommitted/remaining-host-work status is superseded by this snapshot; the raw outcomes and limitations remain valid.
- sanity/: final checks on the exact PR source. These do not repeat the expensive matrices.
