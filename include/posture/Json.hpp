// 手写 JSON 序列化：只要「输出」不要「解析」，够 daemon 写状态与事件用。
// 避免为一行 JSON 引 nlohmann/json（板端无包、无外网）。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace posture {

// 极简 JSON 构造器。Escape 会处理引号、反斜杠、控制字符与 UTF-8 直通。
class Json {
 public:
  Json() { body_ = "{"; }

  Json& Str(const std::string& key, const std::string& value);
  Json& Num(const std::string& key, double value, int decimals = 2);
  Json& Int(const std::string& key, long long value);
  Json& Bool(const std::string& key, bool value);
  Json& Raw(const std::string& key, const std::string& raw_json);
  Json& Array(const std::string& key, const std::vector<double>& values, int decimals = 2);
  Json& Strings(const std::string& key, const std::vector<std::string>& values);

  std::string Build() const { return body_ + "}"; }

  static std::string Escape(const std::string& in);
  // 把一个 "a=1 b=2" 之类的键值对列表拼成对象（调试用）
  static std::string FormatNumber(double value, int decimals);

 private:
  void Prefix(const std::string& key);
  std::string body_;
};

}  // namespace posture
