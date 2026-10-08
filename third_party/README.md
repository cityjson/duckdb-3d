# Vendored third-party code

| Path | Upstream | Version | Licence |
| --- | --- | --- | --- |
| `mapbox/earcut.hpp` | [mapbox/earcut.hpp](https://github.com/mapbox/earcut.hpp) | 2.2.4 | ISC (`mapbox/LICENSE`) |

Copied unmodified. Kept outside `src/` so the format and tidy gates, which cover `src/` and
`test/`, do not reformat or lint it. `earcut.hpp` triangulates a face's rings in
`src/kernel/triangulation.cpp`.
