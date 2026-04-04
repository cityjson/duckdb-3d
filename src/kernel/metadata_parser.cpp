#include "kernel/metadata_parser.hpp"
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace duckdb_3d {

namespace {

//! Minimal JSON value extraction helpers.
//! These are intentionally simple — we only need to extract a few known fields
//! from a flat JSON object. Not a general-purpose JSON parser.

//! Skip whitespace
size_t SkipWS(const std::string &s, size_t pos) {
	while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
		pos++;
	}
	return pos;
}

//! Try to extract a JSON string value for a given key.
//! Returns empty string if key not found.
std::string ExtractStringField(const std::string &json, const std::string &key) {
	std::string search = "\"" + key + "\"";
	auto pos = json.find(search);
	if (pos == std::string::npos) {
		return "";
	}
	pos += search.size();
	pos = SkipWS(json, pos);
	if (pos >= json.size() || json[pos] != ':') {
		return "";
	}
	pos++;
	pos = SkipWS(json, pos);
	if (pos >= json.size() || json[pos] != '"') {
		return "";
	}
	pos++; // skip opening quote
	size_t end = json.find('"', pos);
	if (end == std::string::npos) {
		return "";
	}
	return json.substr(pos, end - pos);
}

//! Try to extract a JSON integer value for a given key.
//! Returns -1 if key not found.
int64_t ExtractIntField(const std::string &json, const std::string &key) {
	std::string search = "\"" + key + "\"";
	auto pos = json.find(search);
	if (pos == std::string::npos) {
		return -1;
	}
	pos += search.size();
	pos = SkipWS(json, pos);
	if (pos >= json.size() || json[pos] != ':') {
		return -1;
	}
	pos++;
	pos = SkipWS(json, pos);

	// Read digits
	size_t start = pos;
	while (pos < json.size() && (std::isdigit(static_cast<unsigned char>(json[pos])) || json[pos] == '-')) {
		pos++;
	}
	if (pos == start) {
		return -1;
	}
	try {
		return std::stoll(json.substr(start, pos - start));
	} catch (...) {
		return -1;
	}
}

//! Try to extract a JSON array of integers for a given key.
//! Returns empty vector if key not found.
std::vector<uint32_t> ExtractIntArrayField(const std::string &json, const std::string &key) {
	std::string search = "\"" + key + "\"";
	auto pos = json.find(search);
	if (pos == std::string::npos) {
		return {};
	}
	pos += search.size();
	pos = SkipWS(json, pos);
	if (pos >= json.size() || json[pos] != ':') {
		return {};
	}
	pos++;
	pos = SkipWS(json, pos);
	if (pos >= json.size() || json[pos] != '[') {
		return {};
	}
	pos++; // skip [

	std::vector<uint32_t> result;
	while (pos < json.size()) {
		pos = SkipWS(json, pos);
		if (pos >= json.size()) {
			break;
		}
		if (json[pos] == ']') {
			break;
		}
		if (json[pos] == ',') {
			pos++;
			continue;
		}
		// Read integer
		size_t start = pos;
		while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
			pos++;
		}
		if (pos > start) {
			result.push_back(static_cast<uint32_t>(std::stoul(json.substr(start, pos - start))));
		} else {
			break;
		}
	}
	return result;
}

} // anonymous namespace

GeometryMetadata ParseGeometryProperties(const std::string &json_text) {
	GeometryMetadata meta;

	if (json_text.empty()) {
		return meta;
	}

	meta.type = ExtractStringField(json_text, "type");
	if (meta.type.empty()) {
		// Also try "cityjsonType"
		meta.type = ExtractStringField(json_text, "cityjsonType");
	}

	int64_t sc = ExtractIntField(json_text, "shellCount");
	if (sc > 0) {
		meta.shell_count = static_cast<uint32_t>(sc);
	}

	int64_t sol = ExtractIntField(json_text, "solidCount");
	if (sol > 0) {
		meta.solid_count = static_cast<uint32_t>(sol);
	}

	meta.shell_face_counts = ExtractIntArrayField(json_text, "shellFaceCounts");

	return meta;
}

} // namespace duckdb_3d
