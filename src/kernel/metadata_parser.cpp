#include "kernel/metadata_parser.hpp"
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace duckdb_3d {

namespace {

class JSONParser {
public:
	explicit JSONParser(std::string_view input_p) : input(input_p) {
	}

	bool End() const {
		return pos >= input.size();
	}

	void SkipWS() {
		while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
			pos++;
		}
	}

	void Expect(char expected, const char *context) {
		SkipWS();
		if (pos >= input.size() || input[pos] != expected) {
			throw std::runtime_error(std::string("geometry_properties JSON: expected ") + context);
		}
		pos++;
	}

	bool Consume(char expected) {
		SkipWS();
		if (pos < input.size() && input[pos] == expected) {
			pos++;
			return true;
		}
		return false;
	}

	char PeekChar() {
		SkipWS();
		return pos < input.size() ? input[pos] : '\0';
	}

	std::string ParseString() {
		std::string result;
		ScanString(&result);
		return result;
	}

	//! Consumes one JSON string, checking its escapes, and appends its decoded
	//! value to `out` unless `out` is null: a skipped value is validated exactly
	//! as a parsed one, without building a copy nobody reads.
	void ScanString(std::string *out) {
		SkipWS();
		if (pos >= input.size() || input[pos] != '"') {
			throw std::runtime_error("geometry_properties JSON: expected string");
		}
		pos++;

		auto emit = [out](char c) {
			if (out) {
				out->push_back(c);
			}
		};
		const char *base = input.data();
		const size_t n = input.size();
		while (true) {
			// Jump to the next quote or escape: everything before it is plain
			// characters, copied (or skipped) as one run.
			size_t stop = n;
			if (const void *quote = std::memchr(base + pos, '"', n - pos)) {
				stop = static_cast<size_t>(static_cast<const char *>(quote) - base);
			}
			if (const void *escape = std::memchr(base + pos, '\\', stop - pos)) {
				stop = static_cast<size_t>(static_cast<const char *>(escape) - base);
			}
			if (out) {
				out->append(base + pos, stop - pos);
			}
			pos = stop;
			if (pos >= n) {
				throw std::runtime_error("geometry_properties JSON: unterminated string");
			}
			if (input[pos++] == '"') {
				return;
			}

			if (pos >= n) {
				throw std::runtime_error("geometry_properties JSON: unterminated escape sequence");
			}

			char escaped = input[pos++];
			switch (escaped) {
			case '"':
			case '\\':
			case '/':
				emit(escaped);
				break;
			case 'b':
				emit('\b');
				break;
			case 'f':
				emit('\f');
				break;
			case 'n':
				emit('\n');
				break;
			case 'r':
				emit('\r');
				break;
			case 't':
				emit('\t');
				break;
			// KNOWN LIMITATION: \uXXXX escapes are validated then replaced with '?' — non-ASCII
			// geometry_properties strings are corrupted. Follow-up: decode to UTF-8 or swap in a
			// real JSON parser.
			case 'u':
				for (int i = 0; i < 4; i++) {
					if (pos >= input.size() || !std::isxdigit(static_cast<unsigned char>(input[pos]))) {
						throw std::runtime_error("geometry_properties JSON: invalid unicode escape");
					}
					pos++;
				}
				emit('?');
				break;
			default:
				throw std::runtime_error("geometry_properties JSON: invalid escape sequence");
			}
		}
	}

	int64_t ParseInteger() {
		SkipWS();
		size_t start = pos;
		if (pos < input.size() && input[pos] == '-') {
			pos++;
		}
		size_t digits_start = pos;
		while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
			pos++;
		}
		if (digits_start == pos) {
			throw std::runtime_error("geometry_properties JSON: expected integer");
		}
		if (pos < input.size() && (input[pos] == '.' || input[pos] == 'e' || input[pos] == 'E')) {
			throw std::runtime_error("geometry_properties JSON: expected integer");
		}

		try {
			return std::stoll(std::string(input.substr(start, pos - start)));
		} catch (...) {
			throw std::runtime_error("geometry_properties JSON: integer out of range");
		}
	}

	std::vector<uint32_t> ParseUIntArray() {
		Expect('[', "'['");
		std::vector<uint32_t> result;
		if (Consume(']')) {
			return result;
		}

		while (true) {
			int64_t value = ParseInteger();
			if (value < 0) {
				throw std::runtime_error("geometry_properties JSON: expected non-negative integer");
			}
			if (value > std::numeric_limits<uint32_t>::max()) {
				throw std::runtime_error("geometry_properties JSON: shell face count out of range");
			}
			result.push_back(static_cast<uint32_t>(value));
			if (Consume(']')) {
				return result;
			}
			Expect(',', "','");
		}
	}

	//! Parse a `shells` value: LIST<LIST<INT>>, one per-shell face-count array
	//! per solid ([[12]], [[12, 4]], [[12], [8, 4]]). A flat array is not the
	//! format's shape and is rejected rather than guessed at.
	std::vector<std::vector<uint32_t>> ParseShells() {
		Expect('[', "'['");
		std::vector<std::vector<uint32_t>> result;
		if (Consume(']')) {
			return result; // empty
		}
		while (true) {
			if (PeekChar() != '[') {
				throw std::runtime_error("geometry_properties JSON: shells must be an array of per-solid arrays "
				                         "of shell face counts, e.g. [[12]]");
			}
			result.push_back(ParseUIntArray());
			if (Consume(']')) {
				return result;
			}
			Expect(',', "','");
		}
	}

	void SkipValue() {
		SkipWS();
		if (pos >= input.size()) {
			throw std::runtime_error("geometry_properties JSON: unexpected end of input");
		}

		char ch = input[pos];
		if (ch == '"') {
			ScanString(nullptr);
			return;
		}
		if (ch == '{') {
			SkipObject();
			return;
		}
		if (ch == '[') {
			SkipArray();
			return;
		}
		if (ch == 't') {
			SkipLiteral("true");
			return;
		}
		if (ch == 'f') {
			SkipLiteral("false");
			return;
		}
		if (ch == 'n') {
			SkipLiteral("null");
			return;
		}
		SkipNumber();
	}

private:
	void SkipObject() {
		Expect('{', "'{'");
		if (Consume('}')) {
			return;
		}
		while (true) {
			ScanString(nullptr);
			Expect(':', "':'");
			SkipValue();
			if (Consume('}')) {
				return;
			}
			Expect(',', "','");
		}
	}

	void SkipArray() {
		Expect('[', "'['");
		if (Consume(']')) {
			return;
		}
		while (true) {
			SkipValue();
			if (Consume(']')) {
				return;
			}
			Expect(',', "','");
		}
	}

	void SkipLiteral(const char *literal) {
		while (*literal != '\0') {
			if (pos >= input.size() || input[pos] != *literal) {
				throw std::runtime_error("geometry_properties JSON: invalid literal");
			}
			pos++;
			literal++;
		}
	}

	void SkipNumber() {
		size_t start = pos;
		if (input[pos] == '-') {
			pos++;
		}
		while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
			pos++;
		}
		if (pos < input.size() && input[pos] == '.') {
			pos++;
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
		}
		if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
			pos++;
			if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {
				pos++;
			}
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
		}
		if (pos == start) {
			throw std::runtime_error("geometry_properties JSON: invalid value");
		}
	}

	std::string_view input;
	size_t pos = 0;
};

} // anonymous namespace

GeometryMetadata ParseGeometryProperties(std::string_view json_text) {
	GeometryMetadata meta;

	if (json_text.empty()) {
		return meta;
	}

	JSONParser parser(json_text);

	parser.Expect('{', "'{'");
	if (!parser.Consume('}')) {
		while (true) {
			auto key = parser.ParseString();
			parser.Expect(':', "':'");

			if (key == "type") {
				// `type` is the geometry type string. It is informational (shell
				// grouping is driven entirely by `shells`), so a non-string `type`
				// is skipped rather than failing the whole import.
				if (parser.PeekChar() == '"') {
					meta.type = parser.ParseString();
				} else {
					parser.SkipValue();
				}
			} else if (key == "shells") {
				if (parser.PeekChar() == 'n') {
					parser.SkipValue(); // null: the non-solid types carry no shells
				} else {
					meta.shells = parser.ParseShells();
				}
			} else {
				// surfaces, face_semantics and any producer extras are irrelevant
				// to shell grouping.
				parser.SkipValue();
			}

			if (parser.Consume('}')) {
				break;
			}
			parser.Expect(',', "','");
		}
	}

	parser.SkipWS();
	if (!parser.End()) {
		throw std::runtime_error("geometry_properties JSON: unexpected trailing content");
	}

	return meta;
}

} // namespace duckdb_3d
