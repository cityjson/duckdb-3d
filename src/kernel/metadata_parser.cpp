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
	std::string type;
	std::string cityjson_type;
	bool has_shell_count = false;
	bool has_solid_count = false;

	parser.Expect('{', "'{'");
	if (!parser.Consume('}')) {
		while (true) {
			auto key = parser.ParseString();
			parser.Expect(':', "':'");

			if (key == "type") {
				type = parser.ParseString();
			} else if (key == "cityjsonType") {
				cityjson_type = parser.ParseString();
			} else if (key == "shellCount") {
				int64_t value = parser.ParseInteger();
				if (value < 1) {
					throw std::runtime_error("geometry_properties JSON: shellCount must be >= 1");
				}
				meta.shell_count = static_cast<uint32_t>(value);
				has_shell_count = true;
			} else if (key == "solidCount") {
				int64_t value = parser.ParseInteger();
				if (value < 1) {
					throw std::runtime_error("geometry_properties JSON: solidCount must be >= 1");
				}
				meta.solid_count = static_cast<uint32_t>(value);
				has_solid_count = true;
			} else if (key == "shellFaceCounts") {
				meta.shell_face_counts = parser.ParseUIntArray();
			} else {
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

	if (!type.empty() && !cityjson_type.empty() && type != cityjson_type) {
		throw std::runtime_error("geometry_properties JSON: conflicting type fields");
	}

	meta.type = !type.empty() ? type : cityjson_type;
	(void)has_shell_count;
	(void)has_solid_count;
	return meta;
}

} // namespace duckdb_3d
