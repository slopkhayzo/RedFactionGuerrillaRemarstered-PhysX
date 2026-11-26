// OpenRFG64 as a build of rfg.exe: every value looked up by name in the program's own PDB (see the header).
#include "openrfg64_column.h"

#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace openrfg64 {
namespace {

// The DIA values DbgHelp reports (cvconst.h is not part of the Windows SDK's headers).
enum : DWORD {
    kTagFunction = 5, kTagData = 7, kTagUDT = 11, kTagEnum = 12, kTagFunctionType = 13, kTagPointerType = 14,
    kTagArrayType = 15, kTagBaseType = 16, kTagTypedef = 17, kTagBaseClass = 18, kTagFunctionArgType = 20,
};
enum : DWORD { kBtVoid = 1, kBtChar = 2, kBtWChar = 3, kBtInt = 6, kBtUInt = 7, kBtFloat = 8, kBtBool = 10, kBtLong = 13, kBtULong = 14 };

// A DbgHelp session of our own (the game's crash reports use the real process handle); one value per plugin module,
// so two plugins never share one.
HANDLE session() {
    static const HANDLE h = reinterpret_cast<HANDLE>(reinterpret_cast<uintptr_t>(&session) | 1);
    return h;
}
DWORD64 g_base = 0;

struct Sym {
    DWORD64 address;
    ULONG size;
    ULONG tag;
    ULONG type;
};

struct EnumContext {
    const char* name;
    std::vector<Sym>* out;
};

BOOL CALLBACK collect(PSYMBOL_INFO info, ULONG, PVOID context) {
    auto* c = static_cast<EnumContext*>(context);
    if (strcmp(info->Name, c->name) == 0) c->out->push_back({info->Address, info->Size, info->Tag, info->TypeIndex});
    return TRUE;
}

struct SymbolBuffer {
    alignas(SYMBOL_INFO) char bytes[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO* info() {
        memset(bytes, 0, sizeof(bytes));
        auto* i = reinterpret_cast<SYMBOL_INFO*>(bytes);
        i->SizeOfStruct = sizeof(SYMBOL_INFO);
        i->MaxNameLen = MAX_SYM_NAME;
        return i;
    }
};

// The wanted names' symbols, collected in one pass over the module (a SymEnumSymbols per name scans every symbol each
// time: 11 s for rfgrt's 204 names).
std::unordered_map<std::string, std::vector<Sym>> g_index;
bool g_indexed = false;

BOOL CALLBACK index_one(PSYMBOL_INFO info, ULONG, PVOID context) {
    auto* wanted = static_cast<const std::unordered_set<std::string>*>(context);
    std::string name(info->Name, info->NameLen);
    if (wanted->count(name)) g_index[name].push_back({info->Address, info->Size, info->Tag, info->TypeIndex});
    return TRUE;
}

void build_index(const Name* names, size_t count) {
    std::unordered_set<std::string> wanted;
    for (size_t k = 0; k < count; ++k)
        if (names[k].kind == 'f' || names[k].kind == 'v') wanted.insert(names[k].name);
    g_index.clear();
    g_indexed = SymEnumSymbols(session(), g_base, "*", index_one, &wanted) != FALSE;
}

// Every symbol of exactly that name: overloads, and file statics of the same name in several sources.
std::vector<Sym> find(const char* name) {
    if (g_indexed) {
        auto it = g_index.find(name);
        if (it != g_index.end()) return it->second;
    }
    std::vector<Sym> out;
    EnumContext c{name, &out};
    if (!g_indexed) SymEnumSymbols(session(), g_base, name, collect, &c);
    if (out.empty()) {
        SymbolBuffer b;
        SYMBOL_INFO* info = b.info();
        if (SymFromName(session(), name, info)) out.push_back({info->Address, info->Size, info->Tag, info->TypeIndex});
    }
    return out;
}

DWORD info32(ULONG type, IMAGEHLP_SYMBOL_TYPE_INFO what, DWORD fallback = 0) {
    DWORD v = 0;
    return SymGetTypeInfo(session(), g_base, type, what, &v) ? v : fallback;
}

ULONG64 info64(ULONG type, IMAGEHLP_SYMBOL_TYPE_INFO what) {
    ULONG64 v = 0;
    return SymGetTypeInfo(session(), g_base, type, what, &v) ? v : 0;
}

std::string name_of(ULONG type) {
    WCHAR* w = nullptr;
    if (!SymGetTypeInfo(session(), g_base, type, TI_GET_SYMNAME, &w) || w == nullptr) return {};
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    LocalFree(w);
    return buf;
}

std::vector<ULONG> children(ULONG type) {
    const DWORD n = info32(type, TI_GET_CHILDRENCOUNT);
    if (n == 0) return {};
    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + n * sizeof(ULONG));
    auto* p = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    p->Count = n;
    p->Start = 0;
    if (!SymGetTypeInfo(session(), g_base, type, TI_FINDCHILDREN, p)) return {};
    return std::vector<ULONG>(p->ChildId, p->ChildId + n);
}

ULONG underlying(ULONG type) {
    for (int i = 0; i < 8 && info32(type, TI_GET_SYMTAG) == kTagTypedef; ++i) type = info32(type, TI_GET_TYPEID);
    return type;
}

// A parameter's type as the original signatures are written: pointers and references both '*'.
std::string type_str(ULONG type, int depth = 0) {
    if (depth > 12) return "?";
    switch (info32(type, TI_GET_SYMTAG)) {
        case kTagPointerType: return type_str(info32(type, TI_GET_TYPEID), depth + 1) + "*";
        case kTagTypedef: return type_str(info32(type, TI_GET_TYPEID), depth + 1);
        case kTagArrayType: return type_str(info32(type, TI_GET_TYPEID), depth + 1) + "*";
        case kTagBaseType: {
            const DWORD bt = info32(type, TI_GET_BASETYPE);
            const ULONG64 len = info64(type, TI_GET_LENGTH);
            switch (bt) {
                case kBtVoid: return "void";
                case kBtChar: return "char";
                case kBtWChar: return "wchar_t";
                case kBtBool: return "bool";
                case kBtFloat: return len == 8 ? "double" : "float";
                case kBtInt:
                case kBtLong: return len == 8 ? "__int64" : len == 2 ? "short" : len == 1 ? "char" : "int";
                case kBtUInt:
                case kBtULong: return len == 8 ? "unsigned __int64" : len == 2 ? "unsigned short" : len == 1 ? "unsigned char" : "unsigned";
                default: return "base" + std::to_string(bt);
            }
        }
        default: {
            std::string n = name_of(type);
            return n.empty() ? "?" : n;
        }
    }
}

std::string signature_of(ULONG function_type) {
    std::string s = "(";
    bool first = true;
    for (ULONG c : children(function_type)) {
        if (info32(c, TI_GET_SYMTAG) != kTagFunctionArgType) continue;
        if (!first) s += ", ";
        s += type_str(info32(c, TI_GET_TYPEID));
        first = false;
    }
    return s + ")";
}

// The original's spelling to OpenRFG's: rchar -> char, uint -> unsigned, references as pointers, no const or spaces.
std::string normalize(const std::string& sig) {
    std::string out;
    size_t i = 0;
    while (i < sig.size()) {
        if (isalpha(static_cast<unsigned char>(sig[i])) || sig[i] == '_') {
            size_t j = i;
            while (j < sig.size() && (isalnum(static_cast<unsigned char>(sig[j])) || sig[j] == '_' || sig[j] == ':')) ++j;
            std::string word = sig.substr(i, j - i);
            if (word == "rchar") word = "char";
            else if (word == "uint" || word == "uint32") word = "unsigned";
            else if (word == "const" || word == "volatile" || word == "struct" || word == "class" || word == "enum") word.clear();
            out += word;
            i = j;
        } else {
            const char c = sig[i] == '&' ? '*' : sig[i];
            if (c != ' ') out += c;
            ++i;
        }
    }
    for (size_t p = out.find("unsignedint"); p != std::string::npos; p = out.find("unsignedint")) out.replace(p, 11, "unsigned");
    return out;
}

size_t arg_count(const std::string& normalized) {
    return normalized == "()" ? 0 : static_cast<size_t>(std::count(normalized.begin(), normalized.end(), ',')) + 1;
}

// A direct or inherited data member: its offset and type.
bool find_member(ULONG type, const std::string& member, uint32_t& offset, ULONG& member_type, int depth = 0) {
    if (depth > 8) return false;
    const std::vector<ULONG> kids = children(type);
    for (ULONG c : kids)
        if (info32(c, TI_GET_SYMTAG) == kTagData && name_of(c) == member) {
            offset = info32(c, TI_GET_OFFSET);
            member_type = info32(c, TI_GET_TYPEID);
            return true;
        }
    for (ULONG c : kids)
        if (info32(c, TI_GET_SYMTAG) == kTagBaseClass) {
            uint32_t inner = 0;
            if (find_member(info32(c, TI_GET_TYPEID), member, inner, member_type, depth + 1)) {
                offset = info32(c, TI_GET_OFFSET) + inner;
                return true;
            }
        }
    return false;
}

// A member path from a type: a.b.c, x[2], (Base). Fills the offset.
bool path_offset(ULONG type, const char* path, uint32_t& offset, std::string& why) {
    offset = 0;
    std::string p(path);
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('.', i);
        if (p[i] == '(') j = p.find(')', i) == std::string::npos ? std::string::npos : p.find('.', p.find(')', i));
        std::string step = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? p.size() : j + 1;
        type = underlying(type);
        if (step.size() > 2 && step.front() == '(' && step.back() == ')') {
            const std::string base = step.substr(1, step.size() - 2);
            bool found = false;
            for (ULONG c : children(type))
                if (info32(c, TI_GET_SYMTAG) == kTagBaseClass && name_of(info32(c, TI_GET_TYPEID)) == base) {
                    offset += info32(c, TI_GET_OFFSET);
                    type = info32(c, TI_GET_TYPEID);
                    found = true;
                    break;
                }
            if (!found) {
                why = "no base class " + base;
                return false;
            }
            continue;
        }
        long index = -1;
        const size_t bracket = step.find('[');
        if (bracket != std::string::npos) {
            index = strtol(step.c_str() + bracket + 1, nullptr, 10);
            step.resize(bracket);
        }
        uint32_t off = 0;
        ULONG member_type = 0;
        if (!find_member(type, step, off, member_type)) {
            why = "no member " + step;
            return false;
        }
        offset += off;
        type = underlying(member_type);
        if (index >= 0) {
            if (info32(type, TI_GET_SYMTAG) != kTagArrayType) {
                why = step + " is not an array";
                return false;
            }
            const ULONG element = underlying(info32(type, TI_GET_TYPEID));
            offset += static_cast<uint32_t>(index * info64(element, TI_GET_LENGTH));
            type = element;
        }
    }
    return true;
}

bool type_index(const char* name, ULONG& type) {
    SymbolBuffer b;
    SYMBOL_INFO* info = b.info();
    if (!SymGetTypeFromName(session(), g_base, name, info)) return false;
    type = info->TypeIndex;
    return true;
}

void add(std::string& report, const char* format, ...) {
    char line[600];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    report += line;
    report += '\n';
}

}  // namespace

bool detected(const char* pdb_path, uint16_t machine) {
    if (machine != IMAGE_FILE_MACHINE_AMD64 || pdb_path == nullptr) return false;
    const char* file = pdb_path;
    for (const char* p = pdb_path; *p; ++p)
        if (*p == '\\' || *p == '/') file = p + 1;
    return _stricmp(file, "OpenRFG.pdb") == 0;
}

int load(Name* names, size_t count, uintptr_t base, std::string& report) {
    const ULONGLONG started = GetTickCount64();
    wchar_t exe_w[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_w, MAX_PATH);
    char exe[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, exe_w, -1, exe, sizeof(exe), nullptr, nullptr);
    std::string dir(exe);
    dir = dir.substr(0, dir.find_last_of("\\/"));

    const DWORD options = SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS;
    SymSetOptions(options);
    if (!SymInitialize(session(), dir.c_str(), FALSE)) {
        add(report, "DbgHelp: SymInitialize failed (error %lu)", GetLastError());
        return static_cast<int>(count);
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_base = SymLoadModuleEx(session(), nullptr, exe, nullptr, base, nt->OptionalHeader.SizeOfImage, nullptr, 0);
    if (g_base == 0) {
        add(report, "DbgHelp: cannot load the symbols of %s (error %lu): is OpenRFG.pdb beside it?", exe, GetLastError());
        SymCleanup(session());
        return static_cast<int>(count);
    }

    build_index(names, count);
    int missing = 0;
    bool any_public = false;
    for (size_t k = 0; k < count; ++k) {
        Name& s = names[k];
        *s.value = 0;
        if (s.fn_size) *s.fn_size = 0;
        if (s.kind == 'p') {
            any_public = true;
            continue;  // below, with the public symbols' decorated names
        }
        if (s.kind == 'f' || s.kind == 'v') {
            std::vector<Sym> found = find(s.name);
            if (s.kind == 'f') {
                std::vector<Sym> fns;
                for (const Sym& f : found)
                    if (f.tag == kTagFunction) fns.push_back(f);
                if (!fns.empty()) found = fns;
            }
            if (found.size() > 1 && s.signature) {
                // Overloads: the one whose parameters read like the original's, else the only one with as many.
                const std::string want = normalize(s.signature);
                std::vector<Sym> exact, same_count;
                for (const Sym& f : found) {
                    const std::string have = normalize(signature_of(f.type));
                    if (have == want) exact.push_back(f);
                    if (arg_count(have) == arg_count(want)) same_count.push_back(f);
                }
                if (exact.size() == 1) found = exact;
                else if (same_count.size() == 1) found = same_count;
            }
            if (found.size() != 1) {
                ++missing;
                add(report, "  %s %s: %s", s.kind == 'f' ? "fn" : "var", s.name,
                    found.empty() ? "not in this build" : "ambiguous (several of that name)");
                continue;
            }
            *s.value = static_cast<uint32_t>(found[0].address - g_base);
            if (s.fn_size) *s.fn_size = found[0].size;
            continue;
        }
        ULONG type = 0;
        if (!type_index(s.name, type)) {
            ++missing;
            add(report, "  %s %s%s%s: no such type", s.kind == 'm' ? "field" : "size", s.name, s.member ? "::" : "", s.member ? s.member : "");
            continue;
        }
        if (s.kind == 's') {
            *s.value = static_cast<uint32_t>(info64(type, TI_GET_LENGTH) + s.addend);
            continue;
        }
        uint32_t offset = 0;
        std::string why;
        if (!path_offset(type, s.member, offset, why)) {
            ++missing;
            add(report, "  field %s::%s: %s", s.name, s.member, why.c_str());
            continue;
        }
        *s.value = offset + static_cast<uint32_t>(s.addend);
    }

    if (any_public) {
        // Public symbols by decorated name (vftables): undecorated they can be ambiguous (a class with several bases
        // has a vftable per base under one name), so the decorated publics are read, without undecoration.
        SymSetOptions((options & ~SYMOPT_UNDNAME) | SYMOPT_PUBLICS_ONLY);
        for (size_t k = 0; k < count; ++k) {
            Name& s = names[k];
            if (s.kind != 'p') continue;
            SymbolBuffer b;
            SYMBOL_INFO* info = b.info();
            bool ok = SymFromName(session(), s.name, info) != FALSE;
            if (!ok) {
                // By the undecorated name, each candidate confirmed by its decorated name.
                char plain[MAX_SYM_NAME];
                if (UnDecorateSymbolName(s.name, plain, sizeof(plain), UNDNAME_NAME_ONLY)) {
                    SymSetOptions(options);
                    const std::vector<Sym> found = find(plain);
                    SymSetOptions((options & ~SYMOPT_UNDNAME) | SYMOPT_PUBLICS_ONLY);
                    for (const Sym& f : found) {
                        SymbolBuffer fb;
                        SYMBOL_INFO* fi = fb.info();
                        DWORD64 disp = 0;
                        if (SymFromAddr(session(), f.address, &disp, fi) && disp == 0 && strcmp(fi->Name, s.name) == 0) {
                            info = b.info();
                            info->Address = f.address;
                            ok = true;
                            break;
                        }
                    }
                }
            }
            if (!ok) {
                ++missing;
                add(report, "  pub %s: not in this build", s.name);
                continue;
            }
            *s.value = static_cast<uint32_t>(info->Address - g_base);
        }
        SymSetOptions(options);
    }

    SymUnloadModule64(session(), g_base);
    SymCleanup(session());
    add(report, "OpenRFG64: %zu names looked up in %s's PDB in %llu ms, %d missing", count, exe,
        static_cast<unsigned long long>(GetTickCount64() - started), missing);
    return missing;
}

}  // namespace openrfg64
