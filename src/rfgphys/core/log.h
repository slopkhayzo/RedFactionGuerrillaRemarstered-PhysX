#pragma once
#include <string>

namespace logx {

// Opens (or reopens) the log file; lines written before this are kept in memory and flushed into it.
void open(const std::wstring& path);
void write(const char* fmt, ...);
// Last-chance flush from DllMain during process exit: never blocks on the log lock.
void flush_at_exit();

}  // namespace logx

#define LOG(...) ::logx::write(__VA_ARGS__)
