#include "catch.hpp"
#include "kernel/byte_io.hpp"

#include <algorithm>
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

TEST_CASE("ByteWriter writes little-endian bytes independent of the host", "[byte_io]") {
	ByteWriter w;
	w.WriteU16(0x0201);
	w.WriteU32(0x04030201);
	w.WriteF64(1.0);

	// Least-significant byte first, on any host.
	REQUIRE(w.buffer[0] == 0x01);
	REQUIRE(w.buffer[1] == 0x02);
	REQUIRE(w.buffer[2] == 0x01);
	REQUIRE(w.buffer[3] == 0x02);
	REQUIRE(w.buffer[4] == 0x03);
	REQUIRE(w.buffer[5] == 0x04);
	// 1.0 is IEEE-754 0x3FF0000000000000; little-endian that is:
	const std::vector<uint8_t> one_le = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F};
	REQUIRE(std::equal(one_le.begin(), one_le.end(), w.buffer.begin() + 6));
}

TEST_CASE("ByteReader reads back what ByteWriter wrote", "[byte_io]") {
	ByteWriter w;
	w.WriteU16(0x0201);
	w.WriteU32(0x04030201);
	w.WriteF64(2.5);

	ByteReader r(w.buffer.data(), w.buffer.size(), "test: truncated");
	REQUIRE(r.ReadU16() == 0x0201);
	REQUIRE(r.ReadU32() == 0x04030201);
	REQUIRE(r.ReadF64() == 2.5);
}

TEST_CASE("ValidateOffsets accepts a well-formed array and names the payload family", "[byte_io]") {
	std::vector<uint32_t> offsets = {0, 1, 3};
	ValidateOffsets(offsets, 3, "face-ring", "GEOM_3D payload");
	REQUIRE_THROWS_WITH(ValidateOffsets(std::vector<uint32_t> {0, 2, 1}, 1, "face-ring", "GEOM_3D payload"),
	                    Catch::Contains("non-monotonic face-ring offsets"));
}
