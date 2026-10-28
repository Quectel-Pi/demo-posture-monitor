#include "posture/Json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace posture {

void Json::Prefix(const std::string& key) {
  if (body_.size() > 1) body_ += ',';
  body_ += '"';
  body_ += Escape(key);
  body_ += "\":";
}

std::string Json::FormatNumber(double value, int decimals) {
  if (!std::isfinite(value)) return "null";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  return buf;
}

std::string Json::Escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));  // UTF-8 直通
        }
    }
  }
  return out;
}

Json& Json::Str(const std::string& key, const std::string& value) {
  Prefix(key);
  body_ += '"';
  body_ += Escape(value);
  body_ += '"';
  return *this;
}

Json& Json::Num(const std::string& key, double value, int decimals) {
  Prefix(key);
  body_ += FormatNumber(value, decimals);
  return *this;
}

Json& Json::Int(const std::string& key, long long value) {
  Prefix(key);
  body_ += std::to_string(value);
  return *this;
}

Json& Json::Bool(const std::string& key, bool value) {
  Prefix(key);
  body_ += value ? "true" : "false";
  return *this;
}

Json& Json::Raw(const std::string& key, const std::string& raw_json) {
  Prefix(key);
  body_ += raw_json;
  return *this;
}

Json& Json::Array(const std::string& key, const std::vector<double>& values, int decimals) {
  Prefix(key);
  body_ += '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) body_ += ',';
    body_ += FormatNumber(values[i], decimals);
  }
  body_ += ']';
  return *this;
}

Json& Json::Strings(const std::string& key, const std::vector<std::string>& values) {
  Prefix(key);
  body_ += '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) body_ += ',';
    body_ += '"';
    body_ += Escape(values[i]);
    body_ += '"';
  }
  body_ += ']';
  return *this;
}

}  // namespace posture
