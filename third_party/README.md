# Vendored third-party code

| Path | Upstream | Version | Licence |
| --- | --- | --- | --- |
| `mapbox/earcut.hpp` | [mapbox/earcut.hpp](https://github.com/mapbox/earcut.hpp) | 2.2.4 | ISC (`mapbox/LICENSE`) |

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
