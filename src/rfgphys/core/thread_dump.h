#pragma once

// Hang reports: where a thread of the game is, written to the log as lines starting with "hang:" (resolved offline
// with tools/hang_report.py).
namespace thread_dump {

// The thread's instruction pointer and the return addresses found on its stack (a scan: a few may be stale), as
// module+offset. The thread is suspended for the copy of its stack.
void log_thread(unsigned long tid, const char* label);

}  // namespace thread_dump
