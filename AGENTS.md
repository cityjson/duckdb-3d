# C++ Agent Guide

This document provides repository-specific guidance for coding agents and contributors working on the `duckdb-3d` extension.

## Repository Context

- This repository is for a reusable DuckDB extension focused on 3D solid processing.
- The extension is not CityJSON-specific. CityJSON is an upstream integration, not the core architectural boundary.
- The repo currently contains the template `quack` scaffold. The first implementation task is to rename that scaffold to `three_d` and replace the placeholder SQL functions.
- Unless a later packaging alias proves cleaner, use `three_d` as the legal internal DuckDB extension target and entrypoint name during implementation, while keeping the repository/product name `duckdb-3d`.

## Required Reading

You should read [docs/DESIGN_DOC.md](/private/tmp/duckdb-3d/docs/DESIGN_DOC.md) before changing:

- public SQL APIs
- the `SOLID_3D` binary payload format
- validation semantics
- measurement semantics
- the CityJSON interoperability contract

Treat the design doc as the architectural source of truth.

## Development Workflow

This repository follows strict test-driven development.

Every feature and bug fix must use this order:

1. write a failing test
2. implement the smallest change needed to make the test pass
3. refactor while keeping tests green

This is mandatory, not optional.

### TDD Rules

- Do not start implementing a new public function until a failing test exists.
- For bugs, add a regression test first and confirm it fails for the current behavior.
- Prefer one behavior per test change. Avoid large speculative test bundles.
- Keep refactors behavior-preserving and covered by tests.

### Preferred Test Ordering

1. unit test first
2. SQL integration test second
3. implementation third
4. refactor last

## Test Placement

- Use `test/cpp/` for:
  - WKB parsing
  - canonical solid model construction
  - binary payload round-trips
  - validation logic
  - triangulation, area, and volume math
- Use `test/sql/` for:
  - function binding and naming
  - null handling
  - `TRY` behavior
  - SQL-level result contracts
  - integration behavior with `SOLID_3D`

When both are needed, start with the unit test.

## Build And Tooling

Use the standard DuckDB extension workflow unless the repo evolves away from the template:

1. Run `make` once to prepare the DuckDB build environment.
2. Prefer `GEN=ninja make` when available for faster incremental builds.
3. Use `make test_debug` for debug-oriented development and extension testing.
4. Use `make test` for standard SQL test coverage.
5. Add focused C++ tests under `test/cpp/` when introducing kernel logic.

The loadable extension is expected to live under `build/debug/extension/three_d/` after the scaffold is renamed.

## Coding Guidance

- Keep the SQL/vectorized execution layer separate from the geometry kernel.
- Preserve original polygon topology in the canonical model. Triangulation is a derived cache, not the source of truth.
- Avoid silent geometry repair in v1.
- Fail clearly when topology is unsupported or invalid.
- Preserve binary payload compatibility unless the design doc is intentionally updated and the version is bumped.
- Keep CityJSON-specific assumptions out of the core kernel unless they are explicitly part of the documented interoperability layer.

## Contribution Workflow

- Update tests before implementation.
- Update the design doc when changing architecture or public behavior.
- Keep SQL docs, code, and tests aligned.
- Add round-trip tests for any import or export logic.
- Add regression tests for every bug fix.

## Initial Implementation Priorities

After the documentation phase, implementation should proceed in this order:

1. rename the template `quack` scaffold to `three_d`
2. add failing tests for `SOLID_3D` construction and WKB import
3. implement import/export and structural introspection
4. add failing tests for validation functions
5. implement validation and reporting
6. add failing tests for surface area and volume
7. implement measurement functions

## References

- [docs/DESIGN_DOC.md](/private/tmp/duckdb-3d/docs/DESIGN_DOC.md)
- DuckDB extension development docs
- DuckDB `v1.5.x` APIs and type registration behavior
