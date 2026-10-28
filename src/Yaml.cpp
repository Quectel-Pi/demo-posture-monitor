#include "posture/Yaml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace posture {
namespace {

std::string Trim(const std::string& in) {
  size_t b = 0, e = in.size();
  while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
  return in.substr(b, e - b);
}

// 去掉行内注释：只有在引号外的 '#' 才算注释
std::string StripComment(const std::string& line) {
  bool in_single = false, in_double = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\'' && !in_double) in_single = !in_single;
    else if (c == '"' && !in_single) in_double = !in_double;
    else if (c == '#' && !in_single && !in_double) {
      // YAML 里 '#' 前必须有空白才是注释
      if (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))) {
        return line.substr(0, i);
      }
    }
  }
  return line;
}

std::string Unquote(const std::string& in) {
  if (in.size() >= 2) {
    const char f = in.front(), b = in.back();
    if ((f == '"' && b == '"') || (f == '\'' && b == '\'')) {
      return in.substr(1, in.size() - 2);
    }
  }
  return in;
}

}  // namespace

bool ParseBool(const std::string& raw, bool def) {
  std::string v;
  v.reserve(raw.size());
  for (char c : raw) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (v == "true" || v == "yes" || v == "on" || v == "1") return true;
  if (v == "false" || v == "no" || v == "off" || v == "0") return false;
  return def;
}

bool Yaml::Load(const std::string& path, std::string* err) {
  path_ = path;
  std::ifstream ifs(path);
  if (!ifs) {
    if (err) *err = "无法打开配置文件: " + path;
    return false;
  }
  std::ostringstream os;
  os << ifs.rdbuf();
  return Parse(os.str(), err);
}

bool Yaml::Parse(const std::string& text, std::string* err) {
  values_.clear();
  std::istringstream is(text);
  std::string raw_line;
  // 栈里存 (缩进, 键名)。缩进只用空格，遇到 tab 直接判错。
  std::vector<std::pair<int, std::string>> stack;
  int line_no = 0;

  while (std::getline(is, raw_line)) {
    ++line_no;
    if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
    const std::string line = StripComment(raw_line);
    const std::string trimmed = Trim(line);
    if (trimmed.empty()) continue;
    if (trimmed == "---" || trimmed == "...") continue;

    int indent = 0;
    for (char c : line) {
      if (c == ' ') ++indent;
      else if (c == '\t') {
        if (err) *err = "第 " + std::to_string(line_no) + " 行用了 Tab 缩进";
        return false;
      } else break;
    }

    const size_t colon = trimmed.find(':');
    if (colon == std::string::npos) {
      // 不是 key: value，跳过（例如纯文本块）
      continue;
    }
    std::string key = Trim(trimmed.substr(0, colon));
    std::string value = Trim(trimmed.substr(colon + 1));
    if (key.empty()) continue;

    // 弹出比当前缩进更深的层级
    while (!stack.empty() && stack.back().first >= indent) stack.pop_back();

    std::string full;
    for (const auto& frame : stack) {
      if (!full.empty()) full += '.';
      full += frame.second;
    }
    if (!full.empty()) full += '.';
    full += key;

    if (value.empty()) {
      // 这是一个父节点
      stack.emplace_back(indent, key);
      values_[full] = "";
      continue;
    }
    values_[full] = Unquote(value);
  }
  return true;
}

bool Yaml::Has(const std::string& key) const {
  return values_.find(key) != values_.end();
}

std::string Yaml::Str(const std::string& key, const std::string& def) const {
  const auto it = values_.find(key);
  if (it == values_.end() || it->second.empty()) return def;
  return it->second;
}

double Yaml::Num(const std::string& key, double def) const {
  const auto it = values_.find(key);
  if (it == values_.end() || it->second.empty()) return def;
  try {
    return std::stod(it->second);
  } catch (...) {
    return def;
  }
}

int Yaml::Int(const std::string& key, int def) const {
  const auto it = values_.find(key);
  if (it == values_.end() || it->second.empty()) return def;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return def;
  }
}

bool Yaml::Bool(const std::string& key, bool def) const {
  const auto it = values_.find(key);
  if (it == values_.end() || it->second.empty()) return def;
  return ParseBool(it->second, def);
}

std::vector<double> Yaml::Nums(const std::string& key) const {
  std::vector<double> out;
  const auto it = values_.find(key);
  if (it == values_.end()) return out;
  std::string body = it->second;
  if (!body.empty() && body.front() == '[') body.erase(body.begin());
  if (!body.empty() && body.back() == ']') body.pop_back();
  std::istringstream is(body);
  std::string item;
  while (std::getline(is, item, ',')) {
    const std::string t = Trim(Unquote(Trim(item)));
    if (t.empty()) continue;
    try {
      out.push_back(std::stod(t));
    } catch (...) {
      // 忽略坏值
    }
  }
  return out;
}

std::vector<std::string> Yaml::Keys() const {
  std::vector<std::string> out;
  out.reserve(values_.size());
  for (const auto& kv : values_) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace posture
