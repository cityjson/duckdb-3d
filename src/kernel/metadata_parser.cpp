#include "kernel/metadata_parser.hpp"
#include <cctype>
#include <stdexcept>

namespace duckdb_3d {

namespace {

class JSONParser {
public:
	explicit JSONParser(const std::string &input_p) : input(input_p) {
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
		SkipWS();
		if (pos >= input.size() || input[pos] != '"') {
			throw std::runtime_error("geometry_properties JSON: expected string");
		}
		pos++;

		std::string result;
		while (pos < input.size()) {
			char ch = input[pos++];
			if (ch == '"') {
				return result;
			}
			if (ch != '\\') {
				result.push_back(ch);
				continue;
			}

			if (pos >= input.size()) {
				throw std::runtime_error("geometry_properties JSON: unterminated escape sequence");
			}

			char escaped = input[pos++];
			switch (escaped) {
			case '"':
			case '\\':
			case '/':
				result.push_back(escaped);
				break;
			case 'b':
				result.push_back('\b');
				break;
			case 'f':
				result.push_back('\f');
				break;
			case 'n':
				result.push_back('\n');
				break;
			case 'r':
				result.push_back('\r');
				break;
			case 't':
				result.push_back('\t');
				break;
			case 'u':
				for (int i = 0; i < 4; i++) {
					if (pos >= input.size() || !std::isxdigit(static_cast<unsigned char>(input[pos]))) {
						throw std::runtime_error("geometry_properties JSON: invalid unicode escape");
					}
					pos++;
				}
				result.push_back('?');
				break;
			default:
				throw std::runtime_error("geometry_properties JSON: invalid escape sequence");
			}
		}

		throw std::runtime_error("geometry_properties JSON: unterminated string");
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
			return std::stoll(input.substr(start, pos - start));
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
			result.push_back(static_cast<uint32_t>(value));
			if (Consume(']')) {
				return result;
			}
			Expect(',', "','");
		}
	}

	//! Parse a spec §8 `shells` value: a flat array of per-shell face counts for a
	//! Solid (wrapped as a single solid), or a nested array-of-arrays with one
	//! per-shell array per solid for MultiSolid/CompositeSolid.
	std::vector<std::vector<uint32_t>> ParseShells() {
		Expect('[', "'['");
		std::vector<std::vector<uint32_t>> result;
		if (Consume(']')) {
			return result; // empty
		}
		if (PeekChar() == '[') {
			// Nested form: one per-shell array per solid.
			while (true) {
				result.push_back(ParseUIntArray());
				if (Consume(']')) {
					return result;
				}
				Expect(',', "','");
			}
		}
		// Flat form: a single solid's per-shell counts (outer '[' already consumed).
		std::vector<uint32_t> flat;
		while (true) {
			int64_t value = ParseInteger();
			if (value < 0) {
				throw std::runtime_error("geometry_properties JSON: expected non-negative integer");
			}
			flat.push_back(static_cast<uint32_t>(value));
			if (Consume(']')) {
				break;
			}
			Expect(',', "','");
		}
		result.push_back(std::move(flat));
		return result;
	}

	void SkipValue() {
		SkipWS();
		if (pos >= input.size()) {
			throw std::runtime_error("geometry_properties JSON: unexpected end of input");
		}

		char ch = input[pos];
		if (ch == '"') {
			ParseString();
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
			ParseString();
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

	const std::string &input;
	size_t pos = 0;
};

} // anonymous namespace

GeometryMetadata ParseGeometryProperties(const std::string &json_text) {
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
				// Spec §8: `type` is the CityJSON geometry type string. It is now
				// purely informational (shell grouping is driven entirely by
				// `shells`), so a non-string `type` from a pre-spec producer is
				// tolerated by skipping it rather than failing the whole import.
				if (parser.PeekChar() == '"') {
					meta.type = parser.ParseString();
				} else {
					parser.SkipValue();
				}
			} else if (key == "shells") {
				meta.shells = parser.ParseShells();
			} else {
				// surfaces, face_semantics, lod, and any producer extras (spec §8
				// permits additional keys) are irrelevant to shell grouping.
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
