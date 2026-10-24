# Vendored third-party code

| Path | Upstream | Version | Licence |
| --- | --- | --- | --- |
| `mapbox/earcut.hpp` | [mapbox/earcut.hpp](https://github.com/mapbox/earcut.hpp) | 2.2.4 | ISC (`mapbox/LICENSE`) |
| `predicates/predicates.c` | [Shewchuk, robust predicates](https://www.cs.cmu.edu/~quake/robust.html) | May 18, 1996 | Public domain |

Kept outside `src/` so the format and tidy gates, which cover `src/` and `test/`, do not
reformat or lint it.

**One modification**, marked `duckdb-3d` in the file: `splitEarcut`'s recursion is bounded.
Each split recurses into `earcutLinked` on both halves, and on a self-overlapping ring the
splits can nest once per vertex, so a ring arriving from SQL could exhaust the thread's stack
and crash the process. `Earcut::maxSplitDepth` (32) caps the nesting; past it the
triangulation stops and sets `Earcut::splitDepthExceeded`, and `triangulation.cpp` then
rejects the face, which validation reports as degenerate. Real faces stay far below the cap:
across the 3DBAG Delft and railway fixtures (about 134 000 faces) no face nests more than one
split. `earcut.hpp` triangulates a face's rings in
`src/kernel/triangulation.cpp`.

**`predicates/predicates.c` is a trimmed copy** of Jonathan Richard Shewchuk's adaptive
exact predicates. It keeps `exactinit()`, `orient2d()`, `orient3d()` and the expansion
routines they call (`fast_expansion_sum_zeroelim`, `scale_expansion_zeroelim`, `estimate`),
with no arithmetic changed. The function definitions are converted from K&R to prototype
form, which C23 compilers require; everything is `static`, so the names cannot collide with
another library linked into the same DuckDB binary; and the `<stdio.h>`, `<stdlib.h>` and
`<sys/time.h>` includes go with the printing and random-number routines that needed them
(`<sys/time.h>` does not exist on Windows). Three wrappers at the end, `three_d_exactinit`,
`three_d_orient2d` and `three_d_orient3d`, declared in `predicates/predicates.h`, are the
only external symbols. `src/kernel/predicates.cpp` calls them.
