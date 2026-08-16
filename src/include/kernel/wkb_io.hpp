#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace duckdb_3d {

//! Endian-aware WKB read cursor shared by the SOLID_3D and GEOM_3D parsers.
//! Each WKB geometry (top-level or child) begins with a byte-order flag:
//! 1 = little-endian, 0 = big-endian; ReadByteOrder updates `swap_bytes` so
//! subsequent ReadU32/ReadF64 honour the geometry's declared order (the host
//! is assumed little-endian). `truncation_message` preserves each caller's
//! historical error text.
class WkbCursor {
public:
	WkbCursor(const uint8_t *data, size_t size, const char *truncation_message)
	    : data(data), size(size), truncation_message(truncation_message) {
	}

	const uint8_t *data;
	size_t size;
	size_t pos = 0;
	bool swap_bytes = false;
	const char *truncation_message;

	void Require(size_t n) const {
		if (pos + n > size) {
			throw std::runtime_error(truncation_message);
		}
	}
	uint8_t ReadByte() {
		Require(1);
		return data[pos++];
	}
	void ReadByteOrder() {
		swap_bytes = (ReadByte() == 0);
	}
	uint32_t ReadU32() {
		Require(4);
		uint32_t v;
		std::memcpy(&v, data + pos, 4);
		pos += 4;
		if (swap_bytes) {
			v = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
		}
		return v;
	}
	double ReadF64() {
		Require(8);
		double v;
		if (swap_bytes) {
			uint8_t tmp[8];
			for (int i = 0; i < 8; i++) {
				tmp[i] = data[pos + 7 - i];
			}
			std::memcpy(&v, tmp, 8);
		} else {
			std::memcpy(&v, data + pos, 8);
		}
		pos += 8;
		return v;
	}
};

//! Little-endian WKB writer shared by the SOLID_3D and GEOM_3D exporters.
class WkbLEWriter {
public:
	std::vector<uint8_t> buffer;

	void WriteByte(uint8_t v) {
		buffer.push_back(v);
	}
	void WriteU32(uint32_t v) {
		buffer.push_back(v & 0xFF);
		buffer.push_back((v >> 8) & 0xFF);
		buffer.push_back((v >> 16) & 0xFF);
		buffer.push_back((v >> 24) & 0xFF);
	}
	void WriteF64(double v) {
		uint8_t bytes[8];
		std::memcpy(bytes, &v, 8);
		buffer.insert(buffer.end(), bytes, bytes + 8);
	}
	void WriteByteOrder() {
		WriteByte(1); // little-endian
	}
};

} // namespace duckdb_3d
