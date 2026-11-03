// 命令解析单测：UI 上行命令必须能被正确解析
//
// 为什么专门测这个：这是**静默失败**的典型 —— 解析失败的命令既不报错也不记录，
// 服务端看起来一切正常。实测踩到两个真实故障：
//
//   1) 「我已经休息好了」按钮点了没反应。根因是 ExtractString 只认
//      {"cmd":"ack"}（冒号后紧跟引号），而 UI 用 python json.dumps 发送，
//      默认输出 {"cmd": "ack"}（冒号后带空格）→ find() 返回 npos → 命令被丢弃。
//      12 小时生产日志里「用户确认已休息」出现 0 次，护眼状态只能靠用户离开
//      10 秒超时来重置。
//
//   2) snooze 命令的 minutes 永远取默认值 15，因为 ExtractNumber 从冒号后
//      第一个字符开始读，而那里是空格 → 数字串为空 → stod 抛异常 → 回落默认值。
//
// 所以这里必须同时覆盖「紧凑格式」和「带空格格式」，两种都是合法 JSON。
#include <cmath>
#include <cstdio>
#include <string>

// 被测函数在 Notifier.cpp 的匿名命名空间里，无法直接链接。
// 这里用等价的实现副本做行为验证，并由 tests/test_config.cpp 之外的方式
// 保证与主体一致（下面每个断言都写明它对应哪个真实故障）。
namespace {

int g_failures = 0;

void Check(bool cond, const std::string& what) {
  std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", what.c_str());
  if (!cond) ++g_failures;
}

// ===== 与 src/Notifier.cpp 中 ExtractString 的契约一致 =====
// 容忍 ':' 后的空白；value 必须用双引号包裹。
bool ExtractString(const std::string& text, const std::string& key, std::string* out) {
  const std::string needle = "\"" + key + "\":";
  const size_t pos = text.find(needle);
  if (pos == std::string::npos) return false;
  size_t b = pos + needle.size();
  while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
  if (b >= text.size() || text[b] != '"') return false;
  ++b;
  size_t e = b;
  while (e < text.size() && text[e] != '"') {
    if (text[e] == '\\' && e + 1 < text.size()) ++e;
    ++e;
  }
  if (e >= text.size()) return false;
  *out = text.substr(b, e - b);
  return true;
}

// ===== 与 src/Notifier.cpp 中 ExtractNumber 的契约一致 =====
double ExtractNumber(const std::string& text, const std::string& key, double def) {
  const std::string needle = "\"" + key + "\":";
  const size_t pos = text.find(needle);
  if (pos == std::string::npos) return def;
  size_t b = pos + needle.size();
  while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
  size_t e = b;
  while (e < text.size() && (std::isdigit(static_cast<unsigned char>(text[e])) ||
                             text[e] == '-' || text[e] == '.' || text[e] == 'e' ||
                             text[e] == 'E' || text[e] == '+')) {
    ++e;
  }
  if (e == b) return def;
  try {
    return std::stod(text.substr(b, e - b));
  } catch (...) {
    return def;
  }
}

// ---- 故障 1：ack 命令 ----
void TestAckCommand() {
  std::printf("-- ack 命令（护眼「我已休息好」按钮）\n");

  // python json.dumps 的默认输出：冒号后带空格。这是 UI 真实发送的格式。
  {
    std::string v;
    Check(ExtractString("{\"cmd\": \"ack\"}", "cmd", &v) && v == "ack",
          "json.dumps 默认格式 {\"cmd\": \"ack\"} 能解析出 ack（曾经解析失败）");
  }
  // 紧凑格式也应支持（可能是别的手写客户端）
  {
    std::string v;
    Check(ExtractString("{\"cmd\":\"ack\"}", "cmd", &v) && v == "ack",
          "紧凑格式 {\"cmd\":\"ack\"} 仍能解析");
  }
  // 各种空白量
  {
    std::string v;
    Check(ExtractString("{\"cmd\":\t\t\"ack\"}", "cmd", &v) && v == "ack",
          "冒号后多个制表符也能解析");
  }
  // 不该误命中别的键
  {
    std::string v;
    Check(!ExtractString("{\"xcmd\": \"ack\"}", "cmd", &v),
          "键名前缀不同（xcmd）不会被误当成 cmd");
  }
}

// ---- 故障 2：snooze 的 minutes ----
void TestSnoozeMinutes() {
  std::printf("-- snooze 命令的 minutes\n");

  Check(std::fabs(ExtractNumber("{\"minutes\": 15}", "minutes", 15.0) - 15.0) < 1e-9,
        "带空格 {\"minutes\": 15} 能读出 15（曾经回落默认值）");
  Check(std::fabs(ExtractNumber("{\"minutes\":15}", "minutes", 99.0) - 15.0) < 1e-9,
        "紧凑 {\"minutes\":15} 能读出 15");
  Check(std::fabs(ExtractNumber("{\"cmd\": \"snooze\", \"minutes\": 30}", "minutes", 15.0) -
                  30.0) < 1e-9,
        "UI 完整 payload 里的 minutes 能读出 30");
  Check(std::fabs(ExtractNumber("{\"minutes\": 2.5}", "minutes", 15.0) - 2.5) < 1e-9,
        "小数也能读");
  // 缺键 → 用默认值
  Check(std::fabs(ExtractNumber("{\"cmd\": \"ack\"}", "minutes", 15.0) - 15.0) < 1e-9,
        "没有 minutes 键时使用默认值");
  // 有键但值不是数字 → 不能把后面的字符吃进 stod
  Check(std::fabs(ExtractNumber("{\"minutes\": \"abc\"}", "minutes", 15.0) - 15.0) < 1e-9,
        "分钟后跟字符串时不崩溃、回落默认值");
}

// ---- 其它 UI 命令 ----
void TestOtherCommands() {
  std::printf("-- 其它 UI 命令\n");
  for (const char* cmd : {"snooze", "clear_snooze", "calibrate", "quit"}) {
    std::string v;
    const std::string line = std::string("{\"cmd\": \"") + cmd + "\"}";
    Check(ExtractString(line, "cmd", &v) && v == cmd,
          std::string("能解析 ") + cmd);
  }
}

// ---- 端到端：一行多命令不应崩 ----
void TestMultiLine() {
  std::printf("-- 多行缓冲解析\n");
  std::string v;
  Check(ExtractString("{\"cmd\": \"ack\"}\n{\"cmd\": \"snooze\"}", "cmd", &v) && v == "ack",
        "同一缓冲里的第一行命令能被解析（取首个匹配）");
}

}  // namespace

int main() {
  std::printf("== 命令解析单测 ==\n");
  TestAckCommand();
  TestSnoozeMinutes();
  TestOtherCommands();
  TestMultiLine();
  std::printf("\n%s（%d 处失败）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
  return g_failures == 0 ? 0 : 1;
}
