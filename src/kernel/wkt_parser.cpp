#include "kernel/wkt_parser.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace duckdb_3d {

namespace {

// ISO WKB codes of the 2D classes; the Z variants add 1000.
enum WkbClass : uint32_t {
	kPoint = 1,
	kLineString = 2,
	kPolygon = 3,
	kMultiPoint = 4,
	kMultiLineString = 5,
	kMultiPolygon = 6,
	kPolyhedralSurface = 15,
	kTin = 16,
	kTriangle = 17,
};

constexpr uint32_t kEwkbZ = 0x80000000u;
constexpr uint32_t kEwkbSrid = 0x20000000u;

class WktReader {
public:
	WktReader(const char *text, size_t size) : text_(text), size_(size) {
	}

	std::vector<uint8_t> Read() {
		SkipSpace();
		bool has_srid = false;
		uint32_t srid = 0;
		if (MatchWordPrefix("SRID")) {
			Expect('=');
			srid = ReadSrid();
			Expect(';');
			has_srid = true;
		}
		ewkb_ = has_srid;
		const size_t class_at = pos_;
		uint32_t cls = ReadClass();
		ReadDimension();
		WriteHeader(cls, has_srid, srid);
		ReadBody(cls, class_at);
		SkipSpace();
		if (pos_ != size_) {
			Fail("unexpected text after the geometry");
		}
		return std::move(out_);
	}

private:
	[[noreturn]] void Fail(const std::string &what) const {
		throw std::invalid_argument("WKT: " + what + " at offset " + std::to_string(pos_));
	}

	void SkipSpace() {
		while (pos_ < size_ && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
			pos_++;
		}
	}

	bool Peek(char c) {
		SkipSpace();
		return pos_ < size_ && text_[pos_] == c;
	}

	void Expect(char c) {
		if (!Peek(c)) {
			Fail(std::string("expected '") + c + "'");
		}
		pos_++;
	}

	//! The next run of letters, upper-cased; empty when there is none.
	std::string Word() {
		SkipSpace();
		std::string word;
		while (pos_ < size_ && std::isalpha(static_cast<unsigned char>(text_[pos_]))) {
			word.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(text_[pos_]))));
			pos_++;
		}
		return word;
	}

	//! Consume `word` (case-insensitive) when the text continues with it.
	bool MatchWordPrefix(const char *word) {
		SkipSpace();
		const size_t n = strlen(word);
		if (size_ - pos_ < n) {
			return false;
		}
		for (size_t i = 0; i < n; i++) {
			if (std::toupper(static_cast<unsigned char>(text_[pos_ + i])) != word[i]) {
				return false;
			}
		}
		pos_ += n;
		return true;
	}

	uint32_t ReadSrid() {
		SkipSpace();
		const size_t start = pos_;
		uint64_t value = 0;
		while (pos_ < size_ && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
			value = value * 10 + static_cast<uint64_t>(text_[pos_] - '0');
			if (value > std::numeric_limits<int32_t>::max()) {
				Fail("SRID out of range");
			}
			pos_++;
		}
		if (pos_ == start) {
			Fail("expected an SRID");
		}
		return static_cast<uint32_t>(value);
	}

	uint32_t ReadClass() {
		const size_t start = pos_;
		auto word = Word();
		static const struct {
			const char *name;
			uint32_t cls;
		} classes[] = {{"POINT", kPoint},
		               {"LINESTRING", kLineString},
		               {"POLYGON", kPolygon},
		               {"MULTIPOINT", kMultiPoint},
		               {"MULTILINESTRING", kMultiLineString},
		               {"MULTIPOLYGON", kMultiPolygon},
		               {"POLYHEDRALSURFACE", kPolyhedralSurface},
		               {"TIN", kTin},
		               {"TRIANGLE", kTriangle}};
		for (auto &c : classes) {
			if (word == c.name) {
				return c.cls;
			}
		}
		pos_ = start;
		Fail(word.empty() ? "expected a geometry class" : "GEOM_3D has no class " + word);
	}

	//! A Z tag fixes three ordinates; with none the first coordinate decides.
	void ReadDimension() {
		const size_t start = pos_;
		auto word = Word();
		if (word.empty() || word == "EMPTY") {
			pos_ = start;
			return;
		}
		if (word == "Z") {
			ordinates_ = 3;
			return;
		}
		pos_ = start;
		if (word == "M" || word == "ZM") {
			Fail("GEOM_3D holds no M ordinate");
		}
		Fail("expected Z or '('");
	}

	bool ReadEmpty() {
		const size_t start = pos_;
		if (Word() == "EMPTY") {
			return true;
		}
		pos_ = start;
		return false;
	}

	void WriteU32(uint32_t v) {
		for (int i = 0; i < 4; i++) {
			out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
		}
	}

	void WriteF64(double v) {
		uint64_t bits;
		memcpy(&bits, &v, sizeof(bits));
		for (int i = 0; i < 8; i++) {
			out_.push_back(static_cast<uint8_t>(bits >> (8 * i)));
		}
	}

	void WriteHeader(uint32_t cls, bool srid_flag, uint32_t srid) {
		out_.push_back(1);
		if (ewkb_) {
			WriteU32(cls | kEwkbZ | (srid_flag ? kEwkbSrid : 0));
			if (srid_flag) {
				WriteU32(srid);
			}
		} else {
			WriteU32(cls + 1000);
		}
	}

	//! Reserve a count, to be filled by Patch once it is known.
	size_t Reserve() {
		size_t at = out_.size();
		WriteU32(0);
		return at;
	}

	void Patch(size_t at, uint32_t count) {
		for (int i = 0; i < 4; i++) {
			out_[at + i] = static_cast<uint8_t>(count >> (8 * i));
		}
	}

	double Number() {
		SkipSpace();
		const size_t start = pos_;
		while (pos_ < size_ && (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '+' ||
		                        text_[pos_] == '-' || text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
			pos_++;
		}
		std::string token(text_ + start, pos_ - start);
		char *end = nullptr;
		double value = token.empty() ? 0.0 : std::strtod(token.c_str(), &end);
		if (token.empty() || end != token.c_str() + token.size()) {
			pos_ = start;
			Fail("expected a number");
		}
		return value;
	}

	bool AtNumber() {
		SkipSpace();
		return pos_ < size_ && (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '+' ||
		                        text_[pos_] == '-' || text_[pos_] == '.');
	}

	void Coordinate() {
		double xyz[4];
		int n = 0;
		while (AtNumber()) {
			if (n == 4) {
				Fail("too many ordinates");
			}
			xyz[n++] = Number();
		}
		if (ordinates_ == 0) {
			if (n == 2) {
				Fail("GEOM_3D needs a Z ordinate");
			}
			if (n == 4) {
				Fail("GEOM_3D holds no M ordinate");
			}
			ordinates_ = n;
		}
		if (n != ordinates_ || n != 3) {
			Fail(n < 3 ? "expected three ordinates" : "expected ',' or ')'");
		}
		for (int i = 0; i < 3; i++) {
			WriteF64(xyz[i]);
		}
	}

	//! "(c, c, ...)", written as a count and the coordinates.
	void CoordinateList() {
		Expect('(');
		size_t at = Reserve();
		uint32_t count = 0;
		do {
			Coordinate();
			count++;
		} while (Peek(',') && (pos_++, true));
		Expect(')');
		Patch(at, count);
	}

	//! "(ring, ring, ...)", written as a ring count and the rings.
	void RingList() {
		Expect('(');
		size_t at = Reserve();
		uint32_t count = 0;
		do {
			CoordinateList();
			count++;
		} while (Peek(',') && (pos_++, true));
		Expect(')');
		Patch(at, count);
	}

	void ChildHeader(uint32_t cls) {
		out_.push_back(1);
		WriteU32(ewkb_ ? (cls | kEwkbZ) : cls + 1000);
	}

	void ReadBody(uint32_t cls, size_t class_at) {
		if (ReadEmpty()) {
			if (cls == kPoint) {
				for (int i = 0; i < 3; i++) {
					WriteF64(std::numeric_limits<double>::quiet_NaN());
				}
			} else {
				WriteU32(0);
			}
			return;
		}
		switch (cls) {
		case kPoint:
			Expect('(');
			Coordinate();
			Expect(')');
			return;
		case kLineString:
			CoordinateList();
			return;
		case kPolygon:
		case kTriangle:
			RingList();
			return;
		default:
			break;
		}
		// A collection: "(child, child, ...)", each child written whole.
		Expect('(');
		size_t at = Reserve();
		uint32_t count = 0;
		do {
			switch (cls) {
			case kMultiPoint:
				ChildHeader(kPoint);
				// Both "(1 2 3, 4 5 6)" and "((1 2 3), (4 5 6))" are in use.
				if (Peek('(')) {
					pos_++;
					Coordinate();
					Expect(')');
				} else {
					Coordinate();
				}
				break;
			case kMultiLineString:
				ChildHeader(kLineString);
				CoordinateList();
				break;
			case kMultiPolygon:
			case kPolyhedralSurface:
				ChildHeader(kPolygon);
				RingList();
				break;
			case kTin:
				ChildHeader(kTriangle);
				RingList();
				break;
			default:
				pos_ = class_at;
				Fail("unsupported class");
			}
			count++;
		} while (Peek(',') && (pos_++, true));
		Expect(')');
		Patch(at, count);
	}

	const char *text_;
	size_t size_;
	size_t pos_ = 0;
	int ordinates_ = 0;
	bool ewkb_ = false;
	std::vector<uint8_t> out_;
};

} // namespace

std::vector<uint8_t> WktToWkb(const char *text, size_t size) {
	return WktReader(text, size).Read();
}

} // namespace duckdb_3d
