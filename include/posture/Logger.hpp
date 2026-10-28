// 极简日志：写 stdout（systemd 会收进 journald），带墙上时钟与级别。
// 不引第三方库，避免板端缺包。
#pragma once

#include <sstream>
#include <string>

namespace posture {

enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
bool LogEnabled(LogLevel level);

// 输出一行。线程安全（内部加锁），不会阻塞调用方超过一行写盘时间。
void LogWrite(LogLevel level, const std::string& msg);

// 返回 ISO8601 本地时间，如 2026-10-28T16:30:00.123
std::string WallNowString();
// 返回 MSYS 风格单调时钟毫秒数（调试用）
double SteadyNowMs();

template <typename... Args>
std::string Concat(Args&&... args) {
  std::ostringstream os;
  (os << ... << args);
  return os.str();
}

}  // namespace posture

#define PG_LOG(level, ...)                                  \
  do {                                                      \
    if (::posture::LogEnabled(level))                       \
      ::posture::LogWrite(level, ::posture::Concat(__VA_ARGS__)); \
  } while (0)

#define LOG_DEBUG(...) PG_LOG(::posture::LogLevel::kDebug, __VA_ARGS__)
#define LOG_INFO(...) PG_LOG(::posture::LogLevel::kInfo, __VA_ARGS__)
#define LOG_WARN(...) PG_LOG(::posture::LogLevel::kWarn, __VA_ARGS__)
#define LOG_ERROR(...) PG_LOG(::posture::LogLevel::kError, __VA_ARGS__)
