// 配置解析：YAML 的极小子集（只支持本项目 posture.yaml 用到的写法）
//
// 为什么不用 yaml-cpp：板端 Debian 没有该包，且没有外网，为一份配置引一个
// 依赖不划算。支持的语法（覆盖实施方案 §12 的示例配置）：
//
//   # 注释
//   key: scalar                 -> "key"
//   key: "quoted string"        -> "key"
//   parent:                     -> "parent"
//     child: 1                  -> "parent.child"
//   list: [1, 2, 3]             -> "list" = "1,2,3"（用 nums() 取）
//   nested:                     -> "nested"
//     arr: [0.1, 0.2]           -> "nested.arr"
//
// 不支持：锚点、多行字符串、tab 缩进、flow map。遇到不支持的行会记录到
// errors 里但继续解析（配置错误不应该导致设备起不来）。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace posture {

class Yaml {
 public:
  // 加载文件。文件不存在返回 false 并填 err。
  bool Load(const std::string& path, std::string* err);
  // 直接解析文本（测试用）
  bool Parse(const std::string& text, std::string* err);

  bool Has(const std::string& key) const;
  std::string Str(const std::string& key, const std::string& def) const;
  double Num(const std::string& key, double def) const;
  int Int(const std::string& key, int def) const;
  bool Bool(const std::string& key, bool def) const;
  std::vector<double> Nums(const std::string& key) const;
  // 返回所有以 prefix 开头的键（调试/自检用）
  std::vector<std::string> Keys() const;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::map<std::string, std::string> values_;
};

// 把 "true/false/yes/no/on/off/1/0" 解析为 bool，无法识别时用 def
bool ParseBool(const std::string& raw, bool def);

}  // namespace posture
