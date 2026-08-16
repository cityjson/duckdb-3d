#include "catch.hpp"
#include "kernel/wkb_io.hpp"

#include <vector>

using namespace duckdb_3d;

TEST_CASE("WkbCursor reads little- and big-endian values", "[wkb_io]") {
	// 0x01 LE flag then u32=7 LE, then 0x00 BE flag then u32=7 BE.
	std::vector<uint8_t> buf = {1, 7, 0, 0, 0, 0, 0, 0, 0, 7};
	WkbCursor cur(buf.data(), buf.size(), "test: truncated");
	cur.ReadByteOrder();
	REQUIRE_FALSE(cur.swap_bytes);
	REQUIRE(cur.ReadU32() == 7);
	cur.ReadByteOrder();
	REQUIRE(cur.swap_bytes);
	REQUIRE(cur.ReadU32() == 7);
}

TEST_CASE("WkbCursor throws the caller's truncation message", "[wkb_io]") {
	std::vector<uint8_t> buf = {1};
	WkbCursor cur(buf.data(), buf.size(), "test: truncated");
	cur.ReadByteOrder();
	REQUIRE_THROWS_WITH(cur.ReadU32(), Catch::Contains("test: truncated"));
}

TEST_CASE("WkbLEWriter round-trips through WkbCursor", "[wkb_io]") {
	WkbLEWriter w;
	w.WriteByteOrder();
	w.WriteU32(1015);
	w.WriteF64(2.5);
	WkbCursor cur(w.buffer.data(), w.buffer.size(), "test: truncated");
	cur.ReadByteOrder();
	REQUIRE(cur.ReadU32() == 1015);
	REQUIRE(cur.ReadF64() == 2.5);
}
