#include "posture/Logger.hpp"

#include "posture/Types.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace posture {
namespace {

std::mutex g_log_mutex;
LogLevel g_level = LogLevel::kInfo;

const char* LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO ";
    case LogLevel::kWarn: return "WARN ";
    case LogLevel::kError: return "ERROR";
  }
  return "?????";
}

// 板端 root 下偶发日志写 stdout 失败（journald 还没接上），退化到 stderr 兜底
FILE* Target() {
  static FILE* target = stdout;
  return target ? target : stderr;
}

}  // namespace

void SetLogLevel(LogLevel level) { g_level = level; }
LogLevel GetLogLevel() { return g_level; }

bool LogEnabled(LogLevel level) {
  return static_cast<int>(level) >= static_cast<int>(g_level);
}

void LogWrite(LogLevel level, const std::string& msg) {
  const std::string line = WallNowString() + " [" + LevelTag(level) + "] " + msg + "\n";
  std::lock_guard<std::mutex> lock(g_log_mutex);
  FILE* out = Target();
  std::fwrite(line.data(), 1, line.size(), out);
  std::fflush(out);
}

std::string WallNowString() {
  const auto now = WallClock::now();
  const auto t = WallClock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                static_cast<int>(ms < 0 ? ms + 1000 : ms));
  return buf;
}

double SteadyNowMs() {
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now().time_since_epoch()).count();
}

}  // namespace posture
