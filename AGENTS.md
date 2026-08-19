# Agent & Contributor Guide

Workflow and conventions for `duckdb-3d`. **This file holds only rules.** Anything
descriptive — what the functions are, how the payload is laid out, why the architecture is
shaped the way it is — lives in `docs/` and must not be duplicated here.

## Where things are

```
src/
  three_d_extension.cpp   registration: types + scalar functions
  functions/              SQL layer: bind, decode, vectorized execution
  kernel/                 geometry: WKB, topology, triangulation, measurement, CRS
  include/                headers mirroring functions/ and kernel/
test/
  cpp/                    kernel unit tests (no database)
  sql/                    SQL-surface tests (sqllogic)
  data/                   fixtures and frozen oracle values
docs/                     see the map below
```

The kernel must not learn about DuckDB, and must not learn about CityJSON. That boundary is
what keeps the geometry logic testable without a database and replaceable behind the SQL
surface.

**The extension loads as `three_d`, not `duckdb-3d`.** The DuckDB entry macro expands the
name into a C++ symbol, so it must be a valid identifier: the repository is `duckdb-3d`, the
build target / entrypoint / `LOAD` name is `three_d`, and the SQL functions are `ST_3D*`.
Targets DuckDB **v1.5.x**.

```sh
git clone --recurse-submodules …   # DuckDB itself is a submodule
make                               # first build compiles DuckDB too; then incremental
GEN=ninja make                     # much faster, with ninja + ccache installed
```

## Which document says what

| Document | Owns | Update it when |
| --- | --- | --- |
| `docs/FUNCTIONS.md` | Signatures, overloads, return types, null/error behaviour, examples | **Any** change to a public function's shape or behaviour |
| `docs/DESIGN_DOC.md` | Architecture, invariants, type model, payload contract, validation/measurement semantics | An architectural decision or documented invariant changes |
| `docs/EXAMPLE.md` | Narrative walkthrough on real data | The end-to-end story changes |
| `docs/CITYJSON_INTEROP.md` | Composing with the `cityjson` extension; running gated tests | Interop mechanics change |
| `docs/TEST_COVERAGE.md` | Verification strategy: which oracle is authoritative for what | An oracle or fixture changes |
| `docs/FUTURE_WORK.md` | Deferred design decisions and what "done" needs | Something is deferred or picked up |
| `docs/README.md` | Build, test, distribution mechanics | Tooling changes |
| `README.md` | Project overview, a digest of representative functions | The pitch or headline surface changes |

`docs/index.html` publishes `FUNCTIONS.md` as the GitHub Pages site (docsify, no build step —
serve `docs/` and it renders). Point Pages at the `docs/` folder on the default branch.

**Never put function signatures or algorithm listings in `DESIGN_DOC.md`.** That duplication
is what goes stale. Signatures belong in `FUNCTIONS.md`, reasoning belongs in the design doc.

Read `docs/DESIGN_DOC.md` before changing public SQL APIs, the payload format, validation or
measurement semantics, or the interoperability contract.

## Test-driven development

Mandatory, not aspirational. Red, green, refactor:

1. Write a failing test.
2. Write the smallest change that makes it pass.
3. Refactor while green.

- No new public function starts until a failing test exists.
- Every bug fix starts with a regression test that fails first.
- One behaviour per test change; no speculative test bundles.
- Refactors stay behaviour-preserving and covered.

**Placement.** Geometry logic — WKB parsing, model construction, payload round-trips,
validation, triangulation, area/volume math — goes in `test/cpp/`. SQL-surface behaviour —
binding, nulls, `TRY` semantics, result contracts, interop — goes in `test/sql/`. When both
apply, write the unit test first.

```sh
make test_full     # configure + build + every test, no skips
make test          # SQL tests, release
make test_debug    # SQL tests, debug
make test_cpp      # C++ kernel tests (Catch2)
make test_all      # test_debug + test_cpp
```

**`make test_full` is the target to verify against.** It builds, stages the gated extensions,
and runs both suites, so `require cityjson` / `require spatial` tests execute instead of
skipping. It needs network access.

**`make test`, `test_debug`, and `test_all` do not build.** The ci-tools targets run whatever
binary `build/release` / `build/debug` already holds, so a stale build reports results for
stale code. Build first, or use `test_full`.

**`make test_debug` does not run the C++ tests** — it is SQL-only, just in debug. Use
`test_cpp` or `test_all` when you have touched the kernel.

Run one file: `./build/release/test/unittest "test/sql/<name>.test"`.

The `Makefile` exports `THREE_D_TEST_FIXTURES=1`, which gates the `st_aswkb*` test helpers.
Running a built binary directly without it silently skips every test that requires them.

Under the narrower targets, tests gated on `require cityjson` / `require spatial` skip when
those extensions are not registered with the runner. Those skips are expected, not failures —
but a skip under `test_full` means the staging failed and is worth investigating.

## Writing style for docs

- **Document the present, never the past.** No "fixed", "previously broken", "used to be",
  "now correctly handles", "✅ Done". A reader needs to know how it works today, not its
  biography. Put history in commit messages, where it belongs.
- **No status decoration.** Don't annotate sections with progress markers or gap trackers.
- **Verify before you write.** Every example in `docs/FUNCTIONS.md` is executed against a
  real build and its actual output pasted in. Re-run any example you touch; never paste a
  plausible-looking result. Examples use the 3DBAG Delft tile:
  `https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl`.
- **Don't cite section numbers across files** unless the anchor is stable — prefer naming the
  document and the concept.

## Coding guidance

- Keep the SQL/vectorized layer separate from the geometry kernel.
- Preserve original polygon topology; triangulation is a derived cache, never the truth.
- Never silently repair geometry. Fail clearly when topology is unsupported or invalid.
- Keep CityJSON assumptions out of the kernel, outside the documented interop layer.

## Breaking changes

**Breaking changes are welcome.** This is a research prototype with no back-compatibility
burden — prefer a clean design over a compatible one, and don't add deprecation shims or
legacy aliases.

Two obligations remain:

- Bump the payload version when the binary layout changes, so readers reject what they cannot
  parse.
- Update `docs/FUNCTIONS.md` and any affected tests in the **same** change.

## Git hooks

Two hooks in `.githooks/`. Enable both once per clone:

```sh
git config core.hooksPath .githooks
```

**pre-commit** formats staged C++ (`clang-format`) and trims markdown whitespace. It runs on
every commit; bypass with `git commit --no-verify` only when you must.

**pre-push** gates the push on what CI checks: a format check, a debug build, and
`make test_all`. CI triggers on push, so this is where a red pipeline gets caught. A push
carrying no buildable source skips straight through, and the format check is skipped with a
warning when `clang-format` or `black` is missing rather than failing the push. Gated
`cityjson` / `spatial` tests skip here exactly as they do in CI — run `make test_full` by hand
when you want them to execute. Bypass with `git push --no-verify`.

Neither hook runs `clang-tidy`; CI's tidy pass is too heavy for a hook. Run `just tidy`
before a push you expect to be scrutinised.

## Model roles

- **Advisor: Fable.** Consult it before committing to an approach and before declaring work
  complete.
- **Executor: Sonnet or Opus.** Implementation, tests, and edits.
