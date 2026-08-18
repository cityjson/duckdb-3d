# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or
```bash
make test_debug
```

## PostGIS/SFCGAL differential oracle

`sql/postgis_oracle.test` cross-checks the extension's 3D measurements against
PostGIS + SFCGAL reference values frozen in `data/postgis_oracle/golden.csv`. It
runs under `make test` with **no** PostGIS, container, or network (it reads the
frozen ISO WKB and asserts agreement within tolerance; design doc §8.4).

PostGIS is used offline, dev-time only, to regenerate the golden values — see
`data/postgis_oracle/README.md` and the `just oracle-*` recipes. It is never
wired into `just ci`.
