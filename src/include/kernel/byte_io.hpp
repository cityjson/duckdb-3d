#pragma once

#include "kernel/core_types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb_3d {

//! True when the host stores multi-byte values least-significant byte first, so
//! the little-endian wire format and host order coincide and whole arrays can be
//! copied with one `memcpy`. Every Windows target is little-endian; elsewhere the
//! compiler says. Any other host takes the byte-by-byte path.
#if (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) ||      \
    defined(_WIN32)
constexpr bool kLittleEndianHost = true;
#else
constexpr bool kLittleEndianHost = false;
#endif

//! Little-endian byte writer shared by the payload and WKB encoders.
//!
//! Every multi-byte value is emitted least-significant byte first, independent of
//! the host's byte order. `WriteBytes` stays raw for opaque blobs such as magic
//! strings. The array writers copy whole arrays with `memcpy` only on a
//! little-endian host, where that is the wire format; elsewhere each element goes
//! through the per-value writer, so host order can never leak in.
class ByteWriter {
public:
	std::vector<uint8_t> buffer;

	void Reserve(size_t total) {
		buffer.reserve(total);
	}
	void WriteBytes(const void *src, size_t len) {
		if (len == 0) {
			return;
		}
		size_t pos = buffer.size();
		buffer.resize(pos + len);
		std::memcpy(buffer.data() + pos, src, len);
	}
	void WriteByte(uint8_t v) {
		buffer.push_back(v);
	}
	void WriteU16(uint16_t v) {
		uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
		WriteBytes(b, sizeof(b));
	}
	void WriteU32(uint32_t v) {
		uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
		                static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
		WriteBytes(b, sizeof(b));
	}
	void WriteF64(double v) {
		uint64_t bits;
		std::memcpy(&bits, &v, sizeof(bits));
		uint8_t b[8];
		for (int i = 0; i < 8; i++) {
			b[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
		}
		WriteBytes(b, sizeof(b));
	}
	void WriteU32Array(const uint32_t *data, size_t count) {
		if (kLittleEndianHost) {
			WriteBytes(data, count * sizeof(uint32_t));
			return;
		}
		for (size_t i = 0; i < count; i++) {
			WriteU32(data[i]);
		}
	}
	void WriteF64Array(const double *data, size_t count) {
		if (kLittleEndianHost) {
			WriteBytes(data, count * sizeof(double));
			return;
		}
		for (size_t i = 0; i < count; i++) {
			WriteF64(data[i]);
		}
	}
	//! XYZ triples, x then y then z per vertex.
	void WriteVertices(const Vertex3D *data, size_t count) {
		static_assert(sizeof(Vertex3D) == 3 * sizeof(double), "Vertex3D must be three packed doubles");
		if (kLittleEndianHost) {
			WriteBytes(data, count * sizeof(Vertex3D));
			return;
		}
		for (size_t i = 0; i < count; i++) {
			WriteF64(data[i].x);
			WriteF64(data[i].y);
			WriteF64(data[i].z);
		}
	}
	void WriteByteOrder() {
		WriteByte(1); // little-endian
	}
};

//! Little-endian byte reader with bounds checks.
//!
//! `truncation_message` is the caller's historical error text; `Require` throws it
//! verbatim, and `RequireCount` appends the declared-count detail to it.
class ByteReader {
public:
	ByteReader(const uint8_t *data, size_t size, const char *truncation_message)
	    : data(data), size(size), truncation_message(truncation_message) {
	}

	const uint8_t *data;
	size_t size;
	size_t pos = 0;
	//! A string literal: readers are built per row, and owning a copy would
	//! allocate on every one.
	const char *truncation_message;

	size_t Remaining() const {
		return size - pos;
	}

	void Require(size_t n) const {
		if (n > size - pos) {
			throw std::runtime_error(truncation_message);
		}
	}

	//! Reject a declared element count before allocating for it: the elements
	//! cannot possibly fit in the bytes that remain. Guards against a malformed
	//! blob whose header claims a huge count, which would otherwise trigger a
	//! large allocation before the truncated read is detected. `elem_size` is
	//! widened to 64-bit so `count * elem_size` cannot overflow.
	void RequireCount(uint64_t count, uint64_t elem_size, const char *what) const {
		if (count * elem_size > Remaining()) {
			throw std::runtime_error(std::string(truncation_message) + ": declared " + what +
			                         " count exceeds remaining payload size");
		}
	}

	void ReadBytes(void *dst, size_t len) {
		Require(len);
		std::memcpy(dst, data + pos, len);
		pos += len;
	}
	uint8_t ReadByte() {
		Require(1);
		return data[pos++];
	}
	uint16_t ReadU16() {
		Require(2);
		uint16_t v = static_cast<uint16_t>(data[pos]) | static_cast<uint16_t>(data[pos + 1] << 8);
		pos += 2;
		return v;
	}
	uint32_t ReadU32() {
		Require(4);
		uint32_t v;
		if (kLittleEndianHost) {
			std::memcpy(&v, data + pos, sizeof(v));
		} else {
			v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
			    (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
		}
		pos += 4;
		return v;
	}
	double ReadF64() {
		Require(8);
		double v;
		if (kLittleEndianHost) {
			std::memcpy(&v, data + pos, sizeof(v));
		} else {
			uint64_t bits = 0;
			for (int i = 0; i < 8; i++) {
				bits |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
			}
			std::memcpy(&v, &bits, sizeof(v));
		}
		pos += 8;
		return v;
	}
	//! Reads `count` values, or throws the truncation error before reading any
	//! when they do not all fit.
	void ReadU32Array(uint32_t *dst, size_t count) {
		if (kLittleEndianHost) {
			RequireElements(count, sizeof(uint32_t));
			std::memcpy(dst, data + pos, count * sizeof(uint32_t));
			pos += count * sizeof(uint32_t);
			return;
		}
		for (size_t i = 0; i < count; i++) {
			dst[i] = ReadU32();
		}
	}
	//! XYZ triples, as ByteWriter::WriteVertices writes them.
	void ReadVertices(Vertex3D *dst, size_t count) {
		static_assert(sizeof(Vertex3D) == 3 * sizeof(double), "Vertex3D must be three packed doubles");
		if (kLittleEndianHost) {
			RequireElements(count, sizeof(Vertex3D));
			std::memcpy(dst, data + pos, count * sizeof(Vertex3D));
			pos += count * sizeof(Vertex3D);
			return;
		}
		for (size_t i = 0; i < count; i++) {
			dst[i].x = ReadF64();
			dst[i].y = ReadF64();
			dst[i].z = ReadF64();
		}
	}

private:
	//! `Require` for `count` elements of `elem_size` bytes, without overflowing.
	void RequireElements(uint64_t count, uint64_t elem_size) const {
		if (elem_size != 0 && count > Remaining() / elem_size) {
			throw std::runtime_error(truncation_message);
		}
	}
};

//! Endian-aware WKB read cursor shared by the SOLID_3D and GEOM_3D parsers.
//! Each WKB geometry (top-level or child) begins with a byte-order flag:
//! 1 = little-endian, 0 = big-endian; ReadByteOrder updates `swap_bytes` so
//! subsequent ReadU32/ReadF64 honour the geometry's declared order.
class WkbCursor : public ByteReader {
public:
	WkbCursor(const uint8_t *data, size_t size, const char *truncation_message)
	    : ByteReader(data, size, truncation_message) {
	}

	bool swap_bytes = false;

	void ReadByteOrder() {
		swap_bytes = (ReadByte() == 0);
	}
	uint32_t ReadU32() {
		uint32_t v = ByteReader::ReadU32();
		if (swap_bytes) {
			v = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
		}
		return v;
	}
	double ReadF64() {
		if (!swap_bytes) {
			return ByteReader::ReadF64();
		}
		Require(8);
		uint64_t bits = 0;
		for (int i = 0; i < 8; i++) {
			bits = (bits << 8) | data[pos + i];
		}
		pos += 8;
		double v;
		std::memcpy(&v, &bits, sizeof(v));
		return v;
	}
};

//! Validate a boundary-offset array: non-empty, starting at 0, monotonic, ending
//! at `expected_last`. `prefix` names the payload family ("SOLID_3D payload"),
//! `what` the array ("solid-shell"), so each caller keeps its historical wording.
inline void ValidateOffsets(const std::vector<uint32_t> &offsets, uint32_t expected_last, const char *what,
                            const char *prefix) {
	if (offsets.empty()) {
		throw std::runtime_error(std::string(prefix) + ": missing " + what + " offsets");
	}
	if (offsets.front() != 0) {
		throw std::runtime_error(std::string(prefix) + ": invalid " + what + " offsets");
	}
	for (size_t i = 1; i < offsets.size(); i++) {
		if (offsets[i] < offsets[i - 1]) {
			throw std::runtime_error(std::string(prefix) + ": non-monotonic " + what + " offsets");
		}
	}
	if (offsets.back() != expected_last) {
		throw std::runtime_error(std::string(prefix) + ": inconsistent " + what + " offsets");
	}
}

} // namespace duckdb_3d
