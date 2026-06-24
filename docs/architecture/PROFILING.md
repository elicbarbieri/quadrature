# Profiling

- `make debug` (the `dev` preset) builds with `-g` (debug type) and
  `-fno-omit-frame-pointer` (added for Debug builds in `CMakeLists.txt`), so
  call stacks symbolicate cleanly and unwind cheaply.

- Threads are named at their entry points via `quad_set_thread_name()`
  (`include/quadrature/thread_util.h`), which sets the kernel `comm` field.
  Profilers display these as the per-thread row labels:

  | Name         | Thread                                        |
  | ------------ | --------------------------------------------- |
  | `audio-rt`   | PipeWire RT callback (`src/audio/player.c`)   |
  | `decode`     | audio decode pool (`src/audio/cache.c`)       |
  | `indexer`    | indexer main thread (`src/indexer/indexer.c`) |
  | `index-meta` | indexer metadata pool                         |
  | `index-art`  | indexer artwork pool                          |
  | `ui-main`    | GTK/GLib main loop (`src/ui/main.c`)          |

```bash
nix develop
sudo sysctl kernel.perf_event_paranoid=1
make debug                              # or: cmake --build --preset=dev
samply record ./build/dev/quadrature
```

Use the app (play tracks, run an index), then quit it. samply starts a local
web server and opens the **Firefox Profiler** in your default browser with all
threads sampled. Use the call-tree / flame-graph / stack-chart views and the
thread selector (`audio-rt`, `decode`, …) to drill in.
