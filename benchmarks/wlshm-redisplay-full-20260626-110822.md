# wlshm Redisplay Benchmark - Full Run

Run date: 2026-06-26

wlshm commit: `eabea856b108360d402c517fbd5404e12199dddb`

Note: this run used the working tree on top of that commit, including the
uncommitted follow-up fix that keeps `present()` from synchronously bypassing
the frame-callback throttle.

## Command

```sh
guix shell -m manifest.scm niri xorg-server -- \
  env WLSHM_PARENT_WL=wayland-1 \
  test/manual/wlshm/run-redisplay-bench.sh --runs 3
```

The runner started private nested niri instances for Wayland runs, fullscreened
each nested niri parent window by exact PID, then ran the inner Emacs frame
fullscreen.  X11 was measured under Xvfb.

Result TSV:
`/tmp/wlshm-tests/redisplay-bench-20260626-110822/results.tsv`

## Scenario

| Backend | Scenario | Observed frame | Reported scale | Window system | Emacs |
| --- | --- | ---: | ---: | --- | --- |
| wlshm | niri 3840x2160 scale 1.5 | 2560x1440 | | wlshm | 32.0.50 |
| PGTK | niri 3840x2160 scale 1.5 | 2560x1409 | 2.000 | pgtk | 31.0.50 |
| X11 | Xvfb 3840x2160 scale 1.0 | 3832x2156 | | x | 31.0.50 |

## Median FPS

| Workload | wlshm | PGTK | X11 |
| --- | ---: | ---: | ---: |
| line-scroll | 80.411 | 40.189 | 193.322 |
| page-scroll | 82.524 | 31.837 | 152.172 |
| full-redraw | 59.026 | 24.265 | 116.204 |
| typing | 122.836 | 969.216 | 438.273 |
| image-scroll | 90.485 | 28.035 | 206.898 |

## Raw Results

| Backend | Run | Workload | Frames | Seconds | FPS | ms/frame | Frame |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| wlshm | 1 | line-scroll | 800 | 10.170449 | 78.659 | 12.713 | 2560x1440 |
| wlshm | 1 | page-scroll | 400 | 4.861034 | 82.287 | 12.153 | 2560x1440 |
| wlshm | 1 | full-redraw | 300 | 5.443682 | 55.110 | 18.146 | 2560x1440 |
| wlshm | 1 | typing | 600 | 6.091280 | 98.501 | 10.152 | 2560x1440 |
| wlshm | 1 | image-scroll | 500 | 6.434669 | 77.704 | 12.869 | 2560x1440 |
| wlshm | 2 | line-scroll | 800 | 9.948848 | 80.411 | 12.436 | 2560x1440 |
| wlshm | 2 | page-scroll | 400 | 4.847061 | 82.524 | 12.118 | 2560x1440 |
| wlshm | 2 | full-redraw | 300 | 5.082497 | 59.026 | 16.942 | 2560x1440 |
| wlshm | 2 | typing | 600 | 4.884566 | 122.836 | 8.141 | 2560x1440 |
| wlshm | 2 | image-scroll | 500 | 5.525766 | 90.485 | 11.052 | 2560x1440 |
| wlshm | 3 | line-scroll | 800 | 9.892566 | 80.869 | 12.366 | 2560x1440 |
| wlshm | 3 | page-scroll | 400 | 4.756956 | 84.087 | 11.892 | 2560x1440 |
| wlshm | 3 | full-redraw | 300 | 4.940102 | 60.727 | 16.467 | 2560x1440 |
| wlshm | 3 | typing | 600 | 4.657199 | 128.833 | 7.762 | 2560x1440 |
| wlshm | 3 | image-scroll | 500 | 5.273401 | 94.815 | 10.547 | 2560x1440 |
| PGTK | 1 | line-scroll | 800 | 19.906124 | 40.189 | 24.883 | 2560x1409 |
| PGTK | 1 | page-scroll | 400 | 12.563830 | 31.837 | 31.410 | 2560x1409 |
| PGTK | 1 | full-redraw | 300 | 9.953115 | 30.141 | 33.177 | 2560x1409 |
| PGTK | 1 | typing | 600 | 0.251381 | 2386.816 | 0.419 | 2560x1409 |
| PGTK | 1 | image-scroll | 500 | 20.590685 | 24.283 | 41.181 | 2560x1409 |
| PGTK | 2 | line-scroll | 800 | 22.814388 | 35.066 | 28.518 | 2560x1409 |
| PGTK | 2 | page-scroll | 400 | 12.947406 | 30.894 | 32.369 | 2560x1409 |
| PGTK | 2 | full-redraw | 300 | 12.363741 | 24.265 | 41.212 | 2560x1409 |
| PGTK | 2 | typing | 600 | 0.879383 | 682.296 | 1.466 | 2560x1409 |
| PGTK | 2 | image-scroll | 500 | 17.835056 | 28.035 | 35.670 | 2560x1409 |
| PGTK | 3 | line-scroll | 800 | 17.177803 | 46.572 | 21.472 | 2560x1409 |
| PGTK | 3 | page-scroll | 400 | 9.470111 | 42.238 | 23.675 | 2560x1409 |
| PGTK | 3 | full-redraw | 300 | 12.402807 | 24.188 | 41.343 | 2560x1409 |
| PGTK | 3 | typing | 600 | 0.619057 | 969.216 | 1.032 | 2560x1409 |
| PGTK | 3 | image-scroll | 500 | 14.720925 | 33.965 | 29.442 | 2560x1409 |
| X11 | 1 | line-scroll | 800 | 3.424852 | 233.587 | 4.281 | 3832x2156 |
| X11 | 1 | page-scroll | 400 | 2.254845 | 177.396 | 5.637 | 3832x2156 |
| X11 | 1 | full-redraw | 300 | 2.432308 | 123.340 | 8.108 | 3832x2156 |
| X11 | 1 | typing | 600 | 1.336706 | 448.865 | 2.228 | 3832x2156 |
| X11 | 1 | image-scroll | 500 | 2.519405 | 198.460 | 5.039 | 3832x2156 |
| X11 | 2 | line-scroll | 800 | 4.369689 | 183.079 | 5.462 | 3832x2156 |
| X11 | 2 | page-scroll | 400 | 3.262147 | 122.619 | 8.155 | 3832x2156 |
| X11 | 2 | full-redraw | 300 | 3.129190 | 95.871 | 10.431 | 3832x2156 |
| X11 | 2 | typing | 600 | 1.384902 | 433.244 | 2.308 | 3832x2156 |
| X11 | 2 | image-scroll | 500 | 2.416652 | 206.898 | 4.833 | 3832x2156 |
| X11 | 3 | line-scroll | 800 | 4.138170 | 193.322 | 5.173 | 3832x2156 |
| X11 | 3 | page-scroll | 400 | 2.628605 | 152.172 | 6.572 | 3832x2156 |
| X11 | 3 | full-redraw | 300 | 2.581676 | 116.204 | 8.606 | 3832x2156 |
| X11 | 3 | typing | 600 | 1.369010 | 438.273 | 2.282 | 3832x2156 |
| X11 | 3 | image-scroll | 500 | 2.369976 | 210.973 | 4.740 | 3832x2156 |

## Notes

- wlshm completed all three fullscreen runs at 2560x1440.
- wlshm logs for this run were empty.
- X11 is an integer-scale Xvfb baseline, not a fractional Wayland result.
