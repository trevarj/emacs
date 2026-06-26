# wlshm Redisplay Benchmark Report

Run date: 2026-06-26
Repo commit: 18e2533a3db

## Command

```sh
guix shell -m manifest.scm niri xorg-server -- \
  env WLSHM_PARENT_WL=wayland-1 \
  test/manual/wlshm/run-redisplay-bench.sh --runs 3
```

The runner started a private nested niri for each Wayland backend/run, focused
that nested niri window in the parent niri by its PID, toggled it fullscreen,
then started the inner Emacs frame fullscreen.

## Result Files

| Run | TSV |
| --- | --- |
| fullscreen smoke | `/tmp/wlshm-tests/redisplay-bench-20260626-095421/results.tsv` |
| fullscreen full | `/tmp/wlshm-tests/redisplay-bench-20260626-095451/results.tsv` |
| post-fix smoke attempt | `/tmp/wlshm-tests/redisplay-bench-20260626-101637/results.tsv` |

## Fullscreen Geometry

The fullscreen smoke confirmed that nested niri expanded to the full parent
logical size:

| Backend | Observed frame | Reported scale | Window system | Emacs |
| --- | ---: | ---: | --- | --- |
| wlshm | 2560x1440 | | wlshm | 32.0.50 |
| pgtk | 2560x1409 | 2.000 | pgtk | 31.0.50 |

The requested nested output was `3840x2160` at scale `1.5`, but in parent niri
fullscreen the nested window receives the monitor's full logical extent
(`2560x1440`). PGTK still reports monitor scale `2.000`.

## Full Run Outcome

The full 3-run benchmark did not produce wlshm FPS rows. All three fullscreen
wlshm runs were killed before writing a workload result:

| Run | wlshm log |
| --- | --- |
| 1 | `Io error: Broken pipe`; `wl_shm_pool.create_buffer: Broken pipe` |
| 2 | `Io error: Broken pipe`; `wl_buffer.destroy: Broken pipe` |
| 3 | `Io error: Broken pipe`; `wp_viewport.set_source: Broken pipe` |

PGTK and X11 completed.

## Median FPS From Completed Full Runs

| Workload | wlshm | PGTK | X11 |
| --- | ---: | ---: | ---: |
| line-scroll | | 25.725 | 177.935 |
| page-scroll | | 33.297 | 123.528 |
| full-redraw | | 23.261 | 97.014 |
| typing | | 278.360 | 369.449 |
| image-scroll | | 29.101 | 184.213 |

Observed completed-run geometry:

| Backend | Rows | Observed frame | Reported scale | Window system | Emacs |
| --- | ---: | ---: | ---: | --- | --- |
| pgtk | 15 | 2560x1409 | 2.000 | pgtk | 31.0.50 |
| x11 | 15 | 3832x2156 | | x | 31.0.50 |

## Smoke FPS

The one-run smoke is useful only to confirm geometry and basic workload
viability; it uses much smaller frame counts.

| Workload | wlshm | PGTK |
| --- | ---: | ---: |
| line-scroll | 36.027 | 82.573 |
| page-scroll | 48.720 | 173.007 |
| full-redraw | 27.435 | 83.859 |
| typing | 75.082 | 4169.910 |
| image-scroll | 38.780 | 103.326 |

## Notes

- The benchmark image workload uses generated color PPM data through Emacs's
  `pbm` loader, not XPM.
- X11 is an integer-scale `Xvfb` baseline at 3840x2160, not a fractional
  Wayland result.
- The fullscreen wlshm kill is now the highest-signal result from this run:
  wlshm can complete the smoke at `2560x1440`, but the full frame-count
  workload is killed consistently at that size.

## Fix

Root cause: wlshm throttled commits while a frame callback was outstanding, but
it still called `SlotPool::create_buffer` before deciding to coalesce the frame.
`create_buffer` sends a Wayland `wl_shm_pool.create_buffer` request immediately;
replacing the coalesced frame then destroyed that uncommitted `wl_buffer`.  At
fullscreen size the benchmark could generate hundreds of large redraws while
only one frame could be presented, causing a create/destroy request storm and
eventual broken-pipe disconnects.

Fix: pending frames now store one reusable client-side pixel snapshot.  The Rust
backend creates a `wl_buffer`, sets the viewport source, and commits only when
the frame callback or present-deadline path will actually present that snapshot.
This preserves the existing fractional-scale viewport pairing while bounding
Wayland buffer object churn to committed frames.

The benchmark now also writes per-backend progress files such as
`wlshm-1.progress` in the result directory.  If a backend dies before writing a
TSV workload row, the progress file identifies the last completed stage.

## Post-Fix Validation

Build/check validation completed:

```sh
(cd rust/wlshm-backend && \
  PATH=/gnu/store/jmpziv786qim05yvhlazpkxjbym4fm03-profile/bin:$PATH \
  cargo check)
rm -f src/temacs src/emacs && \
  PATH=/gnu/store/jmpziv786qim05yvhlazpkxjbym4fm03-profile/bin:$PATH make -j
env -u EMACSLOADPATH ./src/emacs -Q --batch \
  -l test/manual/wlshm/redisplay-bench.el \
  --eval '(princ "redisplay-bench-load-ok")'
bash -n test/manual/wlshm/run-redisplay-bench.sh
```

The agent-safe nested-niri smoke rerun did not reach Emacs in this sandbox:

```sh
env WLSHM_PARENT_WL=wayland-1 \
  test/manual/wlshm/run-redisplay-bench.sh \
  --wayland-only --runs 1 --frames-scale smoke
```

It failed while starting the private nested compositor:

```text
WaylandError(Connection(NoCompositor))
```

Log: `/tmp/wlshm-tests/redisplay-bench-20260626-101637/niri.log`.
Because the private nested niri failed before any wlshm Emacs process started,
there is no post-fix fullscreen FPS table from this environment yet.
