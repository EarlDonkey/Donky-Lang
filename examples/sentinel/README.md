# Sentinel (Phase 1)

Sentinel is a real-time Task Manager written in Donky, with a web dashboard.
It samples system-wide CPU, memory, disk usage, and a snapshot of the process
table once per second, keeps the last five minutes, and serves a live UI.

## What it is NOT

Sentinel v1 is **NOT** an antivirus, **NOT** a malware detector, and **NOT**
an anomaly detector. It does not learn a baseline (Phase 2) and does not take
actions (Phase 3). It is a monitoring tool. **Do not rely on it for
security.**

## Build and run

From the repository root:

build-exe examples/sentinel/run.donk
./donky_app.exe


Then open <http://localhost:8082/> in a browser.

(On Windows, run `donky_app.exe` directly.)

## Endpoints

- `GET /` — the dashboard (serves `examples/sentinel/index.html`).
- `GET /api/snapshot` — the full ring buffer as JSON (up to 300 samples).
- `GET /api/latest` — just the most recent sample, or `null` before the first
  sample lands.

Anything else returns `404 Not Found` with a plain-text body.

## Platform notes

- Works on Windows and POSIX.
- `disk_info()` and `total_mem()`/`used_mem()` are system-wide; there is no
  per-process disk or per-process memory cap.
- `cpu_usage()` reads 0 on its first call in a process. `run.donk` primes it
  (calls it once, sleeps 1 s) before spawning the collector, so the first
  recorded sample has a real CPU value. If you call `collector_loop()`
  directly without priming, the first sample's CPU will be 0.
- `get_processes()` is the C runtime's Toolhelp32 (/proc) walk. It is called
  exactly once per second, in the collector. Do not increase the rate.

## Architecture

- **Collector** runs on a `spawn` worker (the runtime's 16-thread pool). It
  samples the OS once per second, builds a sample map, appends it to a
  ring-buffer array capped at 300 entries, and publishes the whole buffer via
  `global_set("sentinel_buffer", buf)`.
- **HTTP server** runs on the main thread. It accepts connections and
  `spawn`s one worker per connection (`handle_client`). Each request reads
  the current buffer with `global_get("sentinel_buffer")`.
- **Shared state:** single writer (collector), single reader (server),
  last-write-wins. `donky_map_set` replaces the value word under the GC
  mutex, so the reader sees either the old buffer or the new one — never a
  torn pointer. See the header comment in `collector.donk`.
- The static HTML is read once at startup (top-level `let HTML_CACHE` in
  `sentinel.donk`, republished via `global_set`, re-read in `handle_client`
  via `global_get` — SEMANTICS §11.3).

## Known limitations

- **No per-process CPU.** `cpu_usage()` is system-wide; there is no
  `get_process_cpu(pid)`. The process table is ranked by memory only.
- **No persistence.** The ring buffer resets on restart.
- **No authentication.** The server listens on localhost only; do not
  expose it.
- **No automatic GC.** The ring buffer is capped at 300 samples and the
  per-sample process list at 30 entries, so total retained state is bounded
  (~9000 maps). `gc()` refuses to run while a spawned worker is alive
  (runtime invariant), so the collector does not call it.
- **Uptime display is browser-side.** The sample shape in this build does
  not carry `uptime()`; the dashboard synthesizes an uptime counter from
  timestamps and the browser clock. If you need OS-authoritative uptime,
  add `"uptime": uptime()` to `collect_once()` and read it in `index.html`.

## Phase 2 preview

Future phases will add baseline learning and a permission-gated action loop.
Those are explicitly out of scope for v1.