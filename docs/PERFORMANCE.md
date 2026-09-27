# Performance

IDHMFIS has a real-time core: a 1 kHz engine feeding DAC output threads that must never starve. This document lists the performance budget the architecture is designed around and how to measure each figure on your own hardware.

These are **design targets**. Results depend on CPU, GPU, DAC hardware, stream count and show content, so measure on the machine you will run shows on.

---

## Targets

### Real-time path

| Metric | Target | Why it matters |
|---|---|---|
| Engine tick period | 1.000 ms | One tick covers input, cues, FX, safety and the optimizer |
| Engine tick jitter (p99) | ≤ 200 µs | Jitter shows up as uneven point spacing on the galvos |
| Generator cost per tick | ≤ 200 µs at 30k pps | Leaves most of the tick budget free for FX and safety |
| DAC output period (p99) | ≤ 200 µs | Keeps DAC buffers full without over-queueing latency |
| MPSC command push | ≤ 500 ns | UI and input threads must never stall on the engine |

### Input and I/O latency

| Metric | Target |
|---|---|
| Art-Net packet → engine (p99) | ≤ 8 ms |
| Art-Net packet → engine (mean) | ≤ 4 ms |
| Project load, 200 cues | ≤ 500 ms |
| Cold start to interactive UI | ≤ 1.5 s |

### Rendering and streaming

| Metric | Target |
|---|---|
| UI frame rate, idle | ≥ 120 fps |
| UI frame rate, 8 active cues | ≥ 60 fps |
| NDI BGRA → UYVY conversion, 1080p | ≤ 5 ms |
| NDI dropped frames at 1080p60 | < 1 per hour |

### Stability

| Metric | Target |
|---|---|
| Memory after a 10-minute session | ≤ 512 MB |
| Memory growth over a 4-hour show | ≤ 5 MB |
| Watchdog restarts over a 4-hour show | 0 |

---

## How the architecture meets these targets

- **No locks on the tick path.** UI and input threads communicate through Cameron314's `concurrentqueue` (MPSC) and `readerwriterqueue` (SPSC). The engine publishes state through a double-buffered snapshot.
- **Absolute-deadline scheduling.** Each tick sleeps until an absolute target time rather than a relative delay, so timing error does not accumulate.
- **Pre-reserved buffers.** Point buffers on the hot path are reserved up front. The expression VM evaluates without allocating.
- **Priority by stream count.** DAC output threads run at `TIME_CRITICAL` for up to four streams and drop to `HIGH` beyond that, so a large rig cannot starve the engine thread.
- **Independent GPU path.** NDI rasterization runs on the GPU and is fed from the render bus, never inline with DAC output.

---

## Measuring

Build in Release first (see [BUILDING.md](BUILDING.md)).

**Benchmark harness.** This measures generator headroom, point-buffer throughput, Art-Net-to-engine latency, FFT cost and project load time:

```powershell
.\build\tests\bench\bench_harness.exe --output bench_results.md
```

**Art-Net latency.** Send animated DMX to a running instance:

```powershell
.\build\bin\artnet_test_sender.exe --ip 127.0.0.1 --universe 0
```

**Cold start.** Time from process launch until `UI_READY` appears in `%AppData%\IDHMFIS\logs\idhmfis.log`.

**Long-run stability.** `tests/bench/soak_test.cpp` runs an accelerated four-hour load: render, Art-Net fuzzing, cue switching, save/load cycles and simulated DAC reconnects, all running together. It reports memory growth, jitter and packet loss.

Share your results by opening an issue with your hardware and the harness output.
