#!/usr/bin/env bash
# Download the benchmark datasets and materialise scripts/bench/bench.py's
# inputs.duckdb in <dir>. Needs network and a locally built cityjson extension
# (CITYJSON_EXTENSION, as for `make test_full`). Helsinki is ~675 MB.
set -euo pipefail

dir=${1:?usage: prepare.sh <output-dir>}
repo=$(cd "$(dirname "$0")/../.." && pwd)
duckdb=${DUCKDB:-$repo/build/release/duckdb}
cityjson=${CITYJSON_EXTENSION:-$repo/../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension}

mkdir -p "$dir"
for f in delft.city.jsonl Helsinki_tex.city.jsonl; do
	[ -f "$dir/$f" ] || curl -sSfL -o "$dir/$f" "https://cityjson.open3d.city/cityjsonseq/$f"
done

rm -f "$dir/inputs.duckdb"
"$duckdb" -unsigned "$dir/inputs.duckdb" <<SQL
LOAD '$cityjson';
CREATE TABLE delft AS
  SELECT id, geometry_lod2_2 AS wkb, geometry_properties_lod2_2 AS props,
         to_json(geometry_properties_lod2_2)::VARCHAR AS props_json
  FROM read_cityjsonseq('$dir/delft.city.jsonl') WHERE geometry_lod2_2 IS NOT NULL;
CREATE TABLE helsinki AS
  SELECT id, geometry_lod2_0 AS wkb, geometry_properties_lod2_0 AS props,
         to_json(geometry_properties_lod2_0)::VARCHAR AS props_json
  FROM read_cityjsonseq('$dir/Helsinki_tex.city.jsonl') WHERE geometry_lod2_0 IS NOT NULL;
SELECT 'delft' AS dataset, count(*) AS solids FROM delft
UNION ALL SELECT 'helsinki', count(*) FROM helsinki;
SQL
