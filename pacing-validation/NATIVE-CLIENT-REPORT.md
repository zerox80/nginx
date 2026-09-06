# Native Linux client and Windows/WSL path investigation

Status: the original Windows Chrome/ownCloud OCIS case is not resolved. This report distinguishes the measured curl transfers from the user-reported browser symptom. No PR has been opened or pushed, and the production patch was not changed by these diagnostics.

## Results that are established

- On the same home router and Ethernet access, the native Ryzen Linux receiver consistently downloads HTTP/3 much faster than the WSL laptop using the same curl and QUIC library binaries.
- Native HTTP/3 still trails HTTP/2. A successful transfer or a faster Linux endpoint is not evidence that the original application problem is fixed.
- The earlier controlled near-zero-RTT GSO regression of the prototype was primarily associated with the fixed 10-MTU batch budget and its wakeup/syscall cost. Microsecond RTT measurements alone did not remove it. That result is separate from this real-WAN endpoint comparison.
- Increasing only the WSL client socket receive buffer and disabling only its socket UDP_GRO did not eliminate the slow WAN transfers. No interface offloads, security filters or global sysctls were changed.

## Quiet WAN comparison

Three 16-MiB transfers per cell, new connections, randomized condition/client order, sequential transfers. No qlog, packet capture or nginx debug logging in this table. Every one of the 48 transfers completed with HTTP 200, the requested protocol and exactly 16 MiB. All observed per-socket receive-drop counters were zero.

| nginx | GSO | HTTP/2 native | HTTP/2 WSL | HTTP/3 native | HTTP/3 WSL |
|---|---|---:|---:|---:|---:|
| stock | off | 425.5 | 436.9 | 295.5 | 35.3 |
| stock | on | 478.5 | 414.2 | 324.7 | 98.2 |
| microsecond pacer | off | 470.2 | 431.0 | 297.3 | 45.8 |
| microsecond pacer | on | 447.3 | 437.2 | 344.8 | 86.9 |

Values are median Mbit/s. The WSL stock HTTP/2 cells include low outliers of 27.8 Mbit/s (GSO off) and 62.1 Mbit/s (GSO on); these were retained. The remaining HTTP/2 runs were predominantly around 400–480 Mbit/s.

### HTTP/3 ranges and CPU cost

CPU values are process CPU seconds per 16-MiB transfer, not CPU percentages. Server CPU is bracketed by SSH snapshots; client CPU uses child-process resource usage. Different client hardware means these CPU times are descriptive, not a controlled CPU architecture comparison.

| nginx | GSO | Client | Mbit/s median [min–max] | Client CPU s | Server CPU s |
|---|---|---|---:|---:|---:|
| stock | off | native | 295.5 [285.4–315.6] | 0.0675 | 0.19 |
| stock | off | wsl | 35.3 [33.9–38.8] | 0.1953 | 0.24 |
| stock | on | native | 324.7 [314.0–330.4] | 0.0602 | 0.11 |
| stock | on | wsl | 98.2 [84.9–277.7] | 0.1066 | 0.12 |
| usec | off | native | 297.3 [289.1–319.7] | 0.0658 | 0.20 |
| usec | off | wsl | 45.8 [40.2–86.7] | 0.2167 | 0.49 |
| usec | on | native | 344.8 [331.0–354.4] | 0.0880 | 0.18 |
| usec | on | wsl | 86.9 [54.1–88.0] | 0.1523 | 0.36 |

The native-client patched medians improve by about 0.6% without GSO and 6.2% with GSO in this small sample. With GSO, server CPU increases from 0.11 to 0.18 seconds per transfer. This does not support a general throughput win or a claim to fix nginx issue #1498.

## Socket GRO diagnostic

A separate 12-transfer pass used stock nginx, GSO off, the same 16-MiB download, and three repetitions per client/GRO condition. The interposer changed only the test client socket UDP_GRO option. The logged effective value confirms 1 for automatic/default and 0 for disabled.

| Client | Socket UDP_GRO | Mbit/s median [min–max] | Client CPU s | Socket drops |
|---|---|---:|---:|---:|
| native | auto | 298.8 [296.5–319.7] | 0.0642 | 0 |
| native | off | 346.9 [340.1–364.4] | 0.1291 | 0 |
| wsl | auto | 29.4 [18.7–162.6] | 0.2066 | 0 |
| wsl | off | 40.8 [27.2–57.8] | 0.1863 | 0 |

Disabling socket GRO did not remove the WSL problem. Native CPU approximately doubled while its speed improved in this separate pass. This does not isolate Windows NIC/driver coalescing or justify disabling system offloads.

## Direct LAN control

The same stock nginx binary ran temporarily on the native PC, serving the WSL laptop on port 8443 with GSO off. Three HTTP/3 results were 281.0, 404.4 and 783.8 Mbit/s; three HTTP/2 results were 887.4, 589.5 and 758.6 Mbit/s. All six completed with zero observed socket drops. The native NIC and qdisc drop counters remained zero.

This excludes a fixed 35-Mbit/s receive or crypto ceiling in this WSL client. It does not establish which part of the WAN/endpoint interaction causes loss. These LAN values are not pooled with WAN values.

## Environment and remaining scope

- Server: the task-owned Hetzner Ubuntu 26.04.1 KVM instance, one nginx worker, static 16-MiB file, TLS 1.3 AES-128-GCM, HTTP/2 and HTTP/3, 1-MiB HTTP/3 stream buffer. No CDN or OCIS proxy/application in the test.
- Native receiver: Ubuntu 26.04.1 Live, Ryzen 5 5600, 32 GiB RAM, r8169 Ethernet at 1 Gbit/s full duplex. The Live overlay was used; internal disks were not modified.
- Laptop receiver: WSL Ubuntu through the Windows Ethernet path, Intel I219-V at 1 Gbit/s. A Fortinet filter is present in Windows; its presence alone is not evidence of fault.
- curl, libcurl, ngtcp2, nghttp3, OpenSSL and other non-libc runtime libraries were copied byte-for-byte to the native client and verified by hashes. Native system libc/kernel and hardware differ from WSL. See runtime/SHA256.json and each metadata.json for exact hashes and environment.
- Eight preliminary native smoke transfers were successful and are retained separately from the 48-transfer comparison. No warmup was removed from the main comparison.
- The user reported severe HTTP/3 slowdown in native Windows Chrome downloading from ownCloud OCIS, with HTTP/2 fast and no CDN. That historical observation is not a WSL-only symptom. This test has not yet reproduced the actual browser/application path.

The three-host packet-capture investigation is documented separately in the root-cause report. Its instrumented throughput must not be merged with this quiet comparison.

## Patch state

No nginx production code or commit structure changed during this endpoint investigation. The current uncommitted production diff remains 13 files, 425 insertions and 17 deletions. Larger burst variants and high-resolution RTT variants remain diagnostic alternatives, not automatic fixes.
