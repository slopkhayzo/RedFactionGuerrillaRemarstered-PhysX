#pragma once
// OpenRFG64: the x86 / x64 differences the diagnostics need (a thread's instruction and stack pointers).
#if defined(_WIN64)
#define RFG_CTX_IP(c) (c).Rip
#define RFG_CTX_SP(c) (c).Rsp
#else
#define RFG_CTX_IP(c) (c).Eip
#define RFG_CTX_SP(c) (c).Esp
#endif
