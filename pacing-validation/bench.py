#!/usr/bin/env python3
"""Stock/patch HTTP/2+HTTP/3 transfer matrix in isolated Linux network namespaces.

Requires root, iproute2, OpenSSL, a curl with HTTP2+HTTP3 and two nginx binaries.
Only namespaces created by this process are changed/deleted. All measurements
(including failures) are preserved. This is not an authoritative WSL benchmark.
"""
import argparse
import csv
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

HERE = Path(__file__).resolve().parent


def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], text=True,
                          capture_output=True, **kwargs)


def checked(argv, **kwargs):
    p = run(argv, **kwargs)
    if p.returncode:
        raise RuntimeError(f"{argv!r}: {p.stderr or p.stdout}")
    return p.stdout


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def quote(path):
    s = str(path)
    if any(c in s for c in '\n\r"$\\'):
        raise ValueError("nginx configuration paths cannot contain quotes, dollar, backslash or newlines")
    return '"' + s + '"'


def stop(proc):
    if proc and proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def cpu_seconds(pid):
    # comm may contain spaces or parentheses; field 3 follows the final ')'.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


class Network:
    def __init__(self, args):
        self.args = args
        tag = uuid.uuid4().hex[:8]
        self.server = f"qp-{tag}-s"
        self.client = f"qp-{tag}-c"
        self.left = f"qp{tag}s"
        self.right = f"qp{tag}c"
        self.created = []
        self.veth_created = False

    def ns(self, server, *argv):
        return ["ip", "netns", "exec", self.server if server else self.client, *argv]

    def __enter__(self):
        try:
            for name in (self.server, self.client):
                checked(["ip", "netns", "add", name])
                self.created.append(name)
            checked(["ip", "link", "add", self.left, "type", "veth",
                     "peer", "name", self.right])
            self.veth_created = True
            for dev, ns, address in ((self.left, self.server, "10.203.0.1/24"),
                                      (self.right, self.client, "10.203.0.2/24")):
                checked(["ip", "link", "set", dev, "netns", ns])
                checked(["ip", "-n", ns, "link", "set", "lo", "up"])
                checked(["ip", "-n", ns, "address", "add", address, "dev", dev])
                checked(["ip", "-n", ns, "link", "set", dev, "mtu",
                         str(self.args.mtu), "up"])
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def set_rtt(self, rtt):
        for server, dev in ((True, self.left), (False, self.right)):
            # These are exclusively owned virtual interfaces.
            checked(self.ns(server, "tc", "qdisc", "replace", "dev", dev,
                            "root", "netem", "limit", str(self.args.queue_packets),
                            "delay", f"{rtt / 2:.6f}ms",
                            "loss", f"{self.args.loss_percent}%",
                            *(["rate", f"{self.args.rate_mbit}mbit"]
                              if self.args.rate_mbit else [])))

    def stats(self):
        result = {}
        for server, dev in ((True, self.left), (False, self.right)):
            result["server" if server else "client"] = json.loads(checked(
                self.ns(server, "tc", "-s", "-j", "qdisc", "show", "dev", dev)))
        return result

    def __exit__(self, *_):
        for name in reversed(self.created):
            run(["ip", "netns", "del", name])
        # A failed move could leave the owned endpoint in the default namespace.
        if self.veth_created:
            run(["ip", "link", "del", self.left])


def config(args, dest, data, cert, key, gso):
    return f"""env PERL5LIB;
daemon off;
master_process off;
worker_processes 1;
pid {quote(dest / 'nginx.pid')};
error_log {quote(dest / 'error.log')} {'debug' if args.debug_log else 'warn'};
events {{ worker_connections 1024; }}
http {{
    access_log off;
    sendfile on;
    http2 on;
    ssl_protocols TLSv1.3;
    ssl_conf_command Ciphersuites TLS_AES_128_GCM_SHA256;
    ssl_certificate {quote(cert)};
    ssl_certificate_key {quote(key)};
    ssl_session_cache off;
    ssl_session_tickets off;
    quic_gso {gso};
    http3_stream_buffer_size {args.stream_buffer};
    server {{
        listen 10.203.0.1:443 ssl;
        listen 10.203.0.1:443 quic reuseport;
        server_name pacing.test;
        location = /file {{ alias {quote(data)}; }}
    }}
}}
"""


def curl_cmd(args, net, cert, protocol):
    return net.ns(False, str(args.curl), "--silent", "--show-error",
                  "--noproxy", "*", "--cacert", str(cert),
                  "--resolve", "pacing.test:443:10.203.0.1",
                  "--http3-only" if protocol == "h3" else "--http2",
                  "--tlsv1.3", "--tls-max", "1.3", "--max-time",
                  str(args.timeout), "--output", "/dev/null")


def one(args, net, binary, variant, protocol, gso, rtt, rep, warmup, seq,
        data, cert, key):
    label = f"{seq:04d}-{variant}-{protocol}-gso{gso}-rtt{rtt:g}-rep{rep}"
    dest = args.out / label
    dest.mkdir()
    (dest / "logs").mkdir()
    server_env = os.environ.copy()
    perl = binary.parent / "src/http/modules/perl/blib"
    if perl.exists():
        server_env["PERL5LIB"] = str(perl / "lib") + ":" + str(perl / "arch")
    conf = dest / "nginx.conf"
    conf.write_text(config(args, dest, data, cert, key, gso))
    cmd = [str(binary), "-p", str(dest) + "/", "-c", str(conf)]
    validation = run(net.ns(True, *cmd, "-t"), env=server_env)
    (dest / "nginx-t.txt").write_text(validation.stdout + validation.stderr)
    if validation.returncode:
        raise RuntimeError(f"nginx -t failed; see {dest}")
    server = capture = None
    row = dict(id=label, variant=variant, protocol=protocol, gso=gso,
               added_rtt_ms=rtt, repeat=rep, warmup=warmup,
               expected_bytes=data.stat().st_size, diagnostics=bool(
                   args.qlog or args.capture or args.debug_log))
    with (dest / "server-console.log").open("w") as console:
        try:
            server = subprocess.Popen(net.ns(True, *cmd),
                                      stdout=console, stderr=console, env=server_env)
            deadline = time.monotonic() + 10
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f"nginx exited; see {dest}")
                health = run(curl_cmd(args, net, cert, "h2") +
                             ["--max-time", "1", "--range", "0-0",
                              "https://pacing.test/file"])
                if health.returncode == 0:
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(f"server readiness failed: {health.stderr}")
                time.sleep(0.05)
            if args.capture and protocol == "h3":
                capture = subprocess.Popen(net.ns(True, "tcpdump", "-n", "-U",
                    "-i", net.left, "-s", "0", "-w", str(dest / "server.pcap"),
                    "udp", "src", "port", "443"), stdout=console, stderr=console)
                time.sleep(0.2)
                if capture.poll() is not None:
                    raise RuntimeError("tcpdump failed to start")
            env = os.environ.copy()
            if args.qlog and protocol == "h3":
                (dest / "qlog").mkdir()
                env["QLOGDIR"] = str(dest / "qlog")
            else:
                env.pop("QLOGDIR", None)
            before_stats = net.stats()
            cpu_before = cpu_seconds(server.pid)
            started = time.monotonic()
            p = run(curl_cmd(args, net, cert, protocol) +
                    ["--write-out", "%{json}", "https://pacing.test/file"], env=env)
            elapsed = time.monotonic() - started
            cpu = cpu_seconds(server.pid) - cpu_before
            save_json(dest / "qdisc.json", dict(before=before_stats, after=net.stats()))
            (dest / "curl.json").write_text(p.stdout + "\n")
            (dest / "curl.stderr").write_text(p.stderr)
            metrics = json.loads(p.stdout) if p.stdout.strip() else {}
            actual_version = str(metrics.get("http_version", ""))
            version_ok = actual_version.startswith("3" if protocol == "h3" else "2")
            size = int(metrics.get("size_download", 0))
            duration = float(metrics.get("time_total", 0))
            row.update(curl_exit=p.returncode,
                       http_status=metrics.get("http_code"),
                       http_version=actual_version, bytes=size,
                       duration_s=duration, wall_duration_s=elapsed,
                       throughput_mbit_s=(size * 8 / duration / 1e6) if duration else None,
                       server_cpu_s=cpu, server_cpu_percent=cpu / elapsed * 100,
                       ok=(p.returncode == 0 and metrics.get("http_code") == 200
                           and size == row["expected_bytes"] and version_ok))
        finally:
            stop(capture)
            stop(server)
    log = (dest / "error.log").read_text(errors="replace")
    rtts = re.findall(r"quic rtt sample latest:\d+ min:\d+ avg:(\d+)", log)
    row.update(server_smoothed_rtt_ms_last=int(rtts[-1]) if rtts else None,
               server_resend_packet_events=log.count("quic resend packet pnum:")
                   if args.debug_log and protocol == "h3" else None,
               server_congestion_loss_events=len(re.findall(
                   r"quic congestion lost t:", log))
                   if args.debug_log and protocol == "h3" else None)
    qlogs = sorted((dest / "qlog").glob("*")) if (dest / "qlog").exists() else []
    qlogs = [p for p in qlogs if p.is_file()]
    row["qlog_files"] = len(qlogs)
    row["client_recovery_packet_lost_events"] = None
    if qlogs:
        summary = run([sys.executable, HERE / "qlog-summary.py", *qlogs])
        (dest / "qlog-summary.jsonl").write_text(summary.stdout)
        (dest / "qlog-summary.stderr").write_text(summary.stderr)
        row["qlog_summary_exit"] = summary.returncode
        if summary.returncode == 0:
            traces = [t for line in summary.stdout.splitlines()
                      for t in json.loads(line)["traces"]]
            clients = [t for t in traces if t["vantage_point"] == "client"]
            if clients:
                row["client_recovery_packet_lost_events"] = sum(
                    t["recovery_packet_lost_events"] for t in clients)
                row["client_smoothed_rtt_native_last"] = clients[-1]["smoothed_rtt_native_last"]
                row["client_rtt_units"] = clients[-1]["rtt_units"]
    if args.capture and protocol == "h3":
        analysis = run([sys.executable, HERE / "packet-gaps.py",
                        dest / "server.pcap", "--mtu", str(args.mtu)])
        (dest / "packet-gaps.json").write_text(analysis.stdout)
        (dest / "packet-gaps.stderr").write_text(analysis.stderr)
        row["packet_gaps_exit"] = analysis.returncode
    save_json(dest / "result.json", row)
    return row


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--stock", required=True, type=Path)
    p.add_argument("--patched", required=True, type=Path)
    p.add_argument("--curl", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--size-mib", type=int, default=256)
    p.add_argument("--rtts", type=float, nargs="+", default=[0, 1, 2, 4, 8, 16, 32])
    p.add_argument("--gso", choices=["off", "on"], nargs="+", default=["off", "on"])
    p.add_argument("--protocols", choices=["h2", "h3"], nargs="+", default=["h2", "h3"])
    p.add_argument("--repetitions", type=int, default=5)
    p.add_argument("--warmups", type=int, default=1)
    p.add_argument("--stream-buffer", default="1m")
    p.add_argument("--mtu", type=int, default=1500)
    p.add_argument("--queue-packets", type=int, default=1000)
    p.add_argument("--rate-mbit", type=float, default=0)
    p.add_argument("--loss-percent", type=float, default=0,
                   help="netem random loss on EACH direction; qlog loss is separate")
    p.add_argument("--timeout", type=int, default=300)
    p.add_argument("--seed", type=int, default=1498)
    p.add_argument("--qlog", action="store_true")
    p.add_argument("--capture", action="store_true")
    p.add_argument("--debug-log", action="store_true")
    args = p.parse_args()
    if os.geteuid() != 0:
        p.error("run as root: isolated network namespaces require CAP_NET_ADMIN")
    if (args.size_mib < 1 or args.repetitions < 1 or args.warmups < 0
            or any(x < 0 for x in args.rtts) or not 1280 <= args.mtu <= 9000
            or not 0 <= args.loss_percent <= 100 or args.rate_mbit < 0
            or args.queue_packets < 1 or args.timeout < 1
            or not re.fullmatch(r"[1-9][0-9]*[kKmM]?", args.stream_buffer)):
        p.error("invalid sizes, rates, delays, counts or stream buffer")
    for name in ("stock", "patched", "curl"):
        path = getattr(args, name).resolve(strict=True)
        if not os.access(path, os.X_OK):
            p.error(f"{name} is not executable")
        setattr(args, name, path)
    for tool in ["ip", "tc", "openssl"] + (["tcpdump", "tshark"] if args.capture else []):
        if shutil.which(tool) is None:
            p.error(f"missing dependency: {tool}")
    args.out = args.out.resolve()
    quote(args.out)
    args.out.mkdir(parents=True, exist_ok=False)
    version = checked([args.curl, "-V"])
    if "HTTP3" not in version or "HTTP2" not in version:
        p.error("curl must support both HTTP2 and HTTP3")
    metadata = dict(started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    kernel=platform.uname()._asdict(), python=sys.version,
                    arguments={k: str(v) if isinstance(v, Path) else v
                               for k, v in vars(args).items()},
                    curl=version, clock_ticks=os.sysconf("SC_CLK_TCK"),
                    warning="WSL is functional validation only. Veth is not a physical NIC. "
                            "Diagnostic logging/capture changes timings; use separate passes.",
                    binaries={})
    for name in ("stock", "patched"):
        binary = getattr(args, name)
        v = run([binary, "-V"])
        metadata["binaries"][name] = dict(path=str(binary), sha256=digest(binary),
                                           version=v.stdout + v.stderr)
    save_json(args.out / "metadata.json", metadata)
    cert, key, data = [args.out / x for x in ("test.crt", "test.key", "file.bin")]
    checked(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=pacing.test",
             "-addext", "subjectAltName=DNS:pacing.test"])
    with data.open("wb") as f:
        f.truncate(args.size_mib * 1024 * 1024)
    # Warm the identical file before the matrix; diagnostic warmup rows are tagged.
    with data.open("rb") as f:
        while f.read(1024 * 1024):
            pass
    rng = random.Random(args.seed)
    rows = []
    try:
        with Network(args) as net:
            for rtt, gso, protocol in itertools.product(args.rtts, args.gso, args.protocols):
                net.set_rtt(rtt)
                for rep in range(-args.warmups, args.repetitions):
                    variants = ["stock", "patched"]
                    rng.shuffle(variants)
                    for variant in variants:
                        row = one(args, net, getattr(args, variant), variant,
                                  protocol, gso, rtt, rep, rep < 0, len(rows),
                                  data, cert, key)
                        rows.append(row)
                        with (args.out / "results.jsonl").open("a") as f:
                            f.write(json.dumps(row) + "\n")
                        print(json.dumps(row), flush=True)
    finally:
        if rows:
            fields = list(dict.fromkeys(k for row in rows for k in row))
            with (args.out / "results.csv").open("w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=fields)
                w.writeheader()
                w.writerows(rows)
    return 0 if all(row["ok"] for row in rows) else 1


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    raise SystemExit(main())
