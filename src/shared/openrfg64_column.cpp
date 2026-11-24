// OpenRFG64 as a build of rfg.exe: every value looked up by name in the program's own PDB (see the header).
#include "openrfg64_column.h"

#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
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
// so two plugins never share one. (A names table tool's process has nothing else.)
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
std::unordered_set<std::string> g_wanted;  // the names the pass looked for
bool g_indexed = false;

BOOL CALLBACK index_one(PSYMBOL_INFO info, ULONG, PVOID context) {
    auto* wanted = static_cast<const std::unordered_set<std::string>*>(context);
    std::string name(info->Name, info->NameLen);
    if (wanted->count(name)) g_index[name].push_back({info->Address, info->Size, info->Tag, info->TypeIndex});
    return TRUE;
}

void build_index(const Name* names, size_t count) {
    g_wanted.clear();
    for (size_t k = 0; k < count; ++k)
        if (names[k].kind == 'f' || names[k].kind == 'v') g_wanted.insert(names[k].name);
    g_index.clear();
    // (a pass over every symbol of the program: none for a list of types and members alone)
    g_indexed = g_wanted.empty() || SymEnumSymbols(session(), g_base, "*", index_one, &g_wanted) != FALSE;
}

// The types' lookups, kept while the module is open: a struct's members are read once, however many of its members a
// list names (most of a layout list's entries share a few types).
std::unordered_map<std::string, ULONG> g_type_by_name;
std::unordered_map<ULONG, std::vector<ULONG>> g_children;
std::unordered_map<ULONG, std::string> g_names;

void clear_type_caches() {
    g_type_by_name.clear();
    g_children.clear();
    g_names.clear();
}

// Every symbol of exactly that name: overloads, and file statics of the same name in several sources.
std::vector<Sym> find(const char* name) {
    if (g_indexed) {
        auto it = g_index.find(name);
        if (it != g_index.end()) return it->second;
        // A wanted name the pass over every symbol did not meet is not in the program: no search by name, which scans
        // the whole PDB again (a third of a second a name: 8 of rfgphys's 10 s with its 25 names a build lacks).
        if (g_wanted.count(name)) return {};
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

const std::string& name_of(ULONG type) {
    auto cached = g_names.find(type);
    if (cached != g_names.end()) return cached->second;
    std::string& name = g_names[type];
    WCHAR* w = nullptr;
    if (SymGetTypeInfo(session(), g_base, type, TI_GET_SYMNAME, &w) && w != nullptr) {
        char buf[512];
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
        LocalFree(w);
        name = buf;
    }
    return name;
}

const std::vector<ULONG>& children(ULONG type) {
    auto cached = g_children.find(type);
    if (cached != g_children.end()) return cached->second;
    std::vector<ULONG>& kids = g_children[type];
    const DWORD n = info32(type, TI_GET_CHILDRENCOUNT);
    if (n == 0) return kids;
    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + n * sizeof(ULONG));
    auto* p = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    p->Count = n;
    p->Start = 0;
    if (SymGetTypeInfo(session(), g_base, type, TI_FINDCHILDREN, p)) kids.assign(p->ChildId, p->ChildId + n);
    return kids;
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
            const std::string& n = name_of(type);
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
    const std::vector<ULONG>& kids = children(type);
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
    auto cached = g_type_by_name.find(name);
    if (cached == g_type_by_name.end()) {
        SymbolBuffer b;
        SYMBOL_INFO* info = b.info();
        cached = g_type_by_name.emplace(name, SymGetTypeFromName(session(), g_base, name, info) ? info->TypeIndex : 0).first;
    }
    type = cached->second;
    return type != 0;
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

// A module's symbols in our session (its PDB beside it), at 'base'.
bool open_module(const char* exe, DWORD64 base, DWORD image_size, std::string& report) {
    std::string dir(exe);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitialize(session(), dir.c_str(), FALSE)) {
        add(report, "DbgHelp: SymInitialize failed (error %lu)", GetLastError());
        return false;
    }
    g_base = SymLoadModuleEx(session(), nullptr, exe, nullptr, base, image_size, nullptr, 0);
    if (g_base == 0) {
        add(report, "DbgHelp: cannot load the symbols of %s (error %lu): is OpenRFG.pdb beside it?", exe, GetLastError());
        SymCleanup(session());
        return false;
    }
    return true;
}

void close_module() {
    SymUnloadModule64(session(), g_base);
    SymCleanup(session());
    g_base = 0;
    clear_type_caches();
    g_index.clear();
    g_wanted.clear();
    g_indexed = false;
}

// Where a lookup's time goes (for the names table tool's report), in ms.
struct Phases {
    ULONGLONG index = 0, symbols = 0, types = 0, publics = 0;
};

// Every entry looked up in the open module. found (may be null) gets 1 for each entry found, 0 for each not.
int resolve(Name* names, size_t count, std::vector<uint8_t>* found_out, std::string& report, Phases* phases = nullptr) {
    const DWORD options = SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS;
    if (found_out) found_out->assign(count, 1);
    auto not_found = [&](size_t k) {
        if (found_out) (*found_out)[k] = 0;
    };
    Phases local;
    Phases& ph = phases ? *phases : local;
    ULONGLONG t = GetTickCount64();
    auto lap = [&t](ULONGLONG& into) {
        const ULONGLONG now = GetTickCount64();
        into += now - t;
        t = now;
    };
    build_index(names, count);
    lap(ph.index);
    bool symbol = false;  // the entry before: a function's or global's
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
        lap(symbol ? ph.symbols : ph.types);  // the entry before's
        symbol = s.kind == 'f' || s.kind == 'v';
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
                not_found(k);
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
            not_found(k);
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
            not_found(k);
            add(report, "  field %s::%s: %s", s.name, s.member, why.c_str());
            continue;
        }
        *s.value = offset + static_cast<uint32_t>(s.addend);
    }
    lap(symbol ? ph.symbols : ph.types);  // the last entry's

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
                not_found(k);
                add(report, "  pub %s: not in this build", s.name);
                continue;
            }
            *s.value = static_cast<uint32_t>(info->Address - g_base);
        }
        SymSetOptions(options);
    }
    lap(ph.publics);
    return missing;
}

// --- the names table file ------------------------------------------------------------------------------------------

constexpr char kTableMagic[8] = {'R', 'F', 'G', 'N', 'A', 'M', 'E', 'S'};
constexpr uint32_t kTableVersion = 1;

struct TableHeader {
    char magic[8];
    uint32_t version;
    uint32_t list_count;
    uint8_t pdb_guid[16];
    uint32_t pdb_age;
    uint32_t reserved;
};
struct TableList {
    uint64_t hash;
    uint32_t count;
    uint32_t missing;
};
struct TableEntry {
    uint32_t value;
    uint32_t fn_size;
    uint32_t found;
};

// The PDB a loaded image names in its debug directory (its CodeView record): GUID and age.
bool image_pdb(uintptr_t image, uint8_t (&guid)[16], uint32_t& age) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(image + dir.VirtualAddress);
    for (DWORD i = 0; dir.VirtualAddress && i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[i].SizeOfData < 24) continue;
        const uint8_t* cv = reinterpret_cast<const uint8_t*>(image + entries[i].AddressOfRawData);
        if (memcmp(cv, "RSDS", 4) != 0) continue;
        memcpy(guid, cv + 4, 16);
        memcpy(&age, cv + 20, 4);
        return true;
    }
    return false;
}

// FNV-1a over what makes an entry what it is, in order: a list's names, not its values.
uint64_t list_hash(const Name* names, size_t count) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    };
    auto text = [&mix](const char* s) {
        if (s) mix(s, strlen(s));
        mix("|", 1);
    };
    for (size_t k = 0; k < count; ++k) {
        const Name& n = names[k];
        mix(&n.kind, 1);
        text(n.name);
        text(n.member);
        text(n.signature);
        const uint8_t sized = n.fn_size ? 1 : 0;
        mix(&sized, 1);
        mix(&n.addend, sizeof(n.addend));
    }
    return h;
}

const char* kind_word(char kind) {
    switch (kind) {
        case 'f': return "fn";
        case 'v': return "var";
        case 'p': return "pub";
        case 'm': return "field";
        default: return "size";
    }
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
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (!open_module(exe, base, nt->OptionalHeader.SizeOfImage, report)) {
        for (size_t k = 0; k < count; ++k) {
            *names[k].value = 0;
            if (names[k].fn_size) *names[k].fn_size = 0;
        }
        return static_cast<int>(count);
    }
    const int missing = resolve(names, count, nullptr, report);
    close_module();
    add(report, "OpenRFG64: %zu names looked up in %s's PDB in %llu ms, %d missing", count, exe,
        static_cast<unsigned long long>(GetTickCount64() - started), missing);
    return missing;
}

int load_table(const List* lists, size_t list_count, uintptr_t base, const wchar_t* path, int* missing, std::string& report) {
    const ULONGLONG started = GetTickCount64();
    char path_u8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path_u8, sizeof(path_u8), nullptr, nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
        add(report, "OpenRFG64: no names table at %s", path_u8);
        return -1;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    fclose(f);
    TableHeader h{};
    uint8_t guid[16];
    uint32_t age = 0;
    if (bytes.size() < sizeof(h)) {
        add(report, "OpenRFG64: the names table %s is cut short", path_u8);
        return -1;
    }
    memcpy(&h, bytes.data(), sizeof(h));
    if (memcmp(h.magic, kTableMagic, sizeof(kTableMagic)) != 0 || h.version != kTableVersion || h.list_count != list_count) {
        add(report, "OpenRFG64: %s is not a names table of this version for these lists", path_u8);
        return -1;
    }
    if (!image_pdb(base, guid, age) || memcmp(guid, h.pdb_guid, 16) != 0 || age != h.pdb_age) {
        add(report, "OpenRFG64: the names table %s was written for another build of the program", path_u8);
        return -1;
    }
    // every list checked before any value is written
    size_t at = sizeof(h);
    std::vector<TableList> heads(list_count);
    for (size_t l = 0; l < list_count; ++l) {
        if (bytes.size() < at + sizeof(TableList)) {
            add(report, "OpenRFG64: the names table %s is cut short", path_u8);
            return -1;
        }
        memcpy(&heads[l], bytes.data() + at, sizeof(TableList));
        at += sizeof(TableList);
        if (heads[l].count != lists[l].count || heads[l].hash != list_hash(lists[l].names, lists[l].count) ||
            bytes.size() < at + size_t(heads[l].count) * sizeof(TableEntry)) {
            add(report, "OpenRFG64: the names table %s was written for other names (list %zu)", path_u8, l);
            return -1;
        }
        at += size_t(heads[l].count) * sizeof(TableEntry);
    }
    at = sizeof(h);
    int total_missing = 0;
    size_t total = 0;
    for (size_t l = 0; l < list_count; ++l) {
        at += sizeof(TableList);
        int list_missing = 0;
        for (size_t k = 0; k < lists[l].count; ++k, at += sizeof(TableEntry)) {
            TableEntry e;
            memcpy(&e, bytes.data() + at, sizeof(e));
            Name& n = lists[l].names[k];
            *n.value = e.found ? e.value : 0;
            if (n.fn_size) *n.fn_size = e.found ? e.fn_size : 0;
            if (!e.found) {
                ++list_missing;
                add(report, "  %s %s%s%s: not in this build (names table)", kind_word(n.kind), n.name, n.member ? "::" : "",
                    n.member ? n.member : "");
            }
        }
        if (missing) missing[l] = list_missing;
        total_missing += list_missing;
        total += lists[l].count;
    }
    add(report, "OpenRFG64: %zu names from the names table %s in %llu ms, %d missing", total, path_u8,
        static_cast<unsigned long long>(GetTickCount64() - started), total_missing);
    return total_missing;
}

int write_table(const List* lists, size_t list_count, const wchar_t* exe, const wchar_t* path, int* missing_out, std::string& report) {
    const ULONGLONG started = GetTickCount64();
    char exe_u8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe_u8, sizeof(exe_u8), nullptr, nullptr);
    // the program mapped as an image (nothing of it runs): its preferred base and size, and the PDB it names
    HMODULE mapped = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mapped) {
        add(report, "names table: cannot map %s (error %lu)", exe_u8, GetLastError());
        return -1;
    }
    const uintptr_t image = reinterpret_cast<uintptr_t>(mapped) & ~uintptr_t(3);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    TableHeader h{};
    memcpy(h.magic, kTableMagic, sizeof(kTableMagic));
    h.version = kTableVersion;
    h.list_count = static_cast<uint32_t>(list_count);
    const bool named = image_pdb(image, h.pdb_guid, h.pdb_age);
    const DWORD64 preferred = nt->OptionalHeader.ImageBase;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    FreeLibrary(mapped);
    if (!named) {
        add(report, "names table: %s names no PDB (no CodeView record)", exe_u8);
        return -1;
    }
    if (!open_module(exe_u8, preferred, size, report)) return -1;
    std::vector<uint8_t> out(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h) + sizeof(h));
    int total_missing = 0;
    size_t total = 0;
    Phases phases;
    for (size_t l = 0; l < list_count; ++l) {
        std::vector<uint8_t> found;
        const int missing = resolve(lists[l].names, lists[l].count, &found, report, &phases);
        TableList head{list_hash(lists[l].names, lists[l].count), static_cast<uint32_t>(lists[l].count), static_cast<uint32_t>(missing)};
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&head), reinterpret_cast<const uint8_t*>(&head) + sizeof(head));
        for (size_t k = 0; k < lists[l].count; ++k) {
            const Name& n = lists[l].names[k];
            TableEntry e{*n.value, n.fn_size ? *n.fn_size : 0, found[k]};
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(&e), reinterpret_cast<const uint8_t*>(&e) + sizeof(e));
        }
        if (missing_out) missing_out[l] = missing;
        total_missing += missing;
        total += lists[l].count;
    }
    close_module();
    // written whole or not at all: a later start never reads half a table
    const std::wstring tmp = std::wstring(path) + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f || fwrite(out.data(), 1, out.size(), f) != out.size()) {
        if (f) fclose(f);
        add(report, "names table: cannot write it beside its path (error %d)", errno);
        return -1;
    }
    fclose(f);
    if (!MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        add(report, "names table: cannot replace the old one (error %lu)", GetLastError());
        return -1;
    }
    add(report, "names table: %zu names of %s in %llu ms, %d missing (symbol index %llu ms, functions and globals %llu ms, "
        "types and members %llu ms, public symbols %llu ms)", total, exe_u8,
        static_cast<unsigned long long>(GetTickCount64() - started), total_missing, static_cast<unsigned long long>(phases.index),
        static_cast<unsigned long long>(phases.symbols), static_cast<unsigned long long>(phases.types),
        static_cast<unsigned long long>(phases.publics));
    return total_missing;
}

int write_table_main(int argc, wchar_t** argv, const List* lists, size_t list_count) {
    if (argc != 3) {
        fprintf(stderr, "usage: <tool> <OpenRFG.exe> <names table>\n");
        return 2;
    }
    std::string report;
    std::vector<int> missing(list_count, 0);
    const int total = write_table(lists, list_count, argv[1], argv[2], missing.data(), report);
    fputs(report.c_str(), stdout);
    if (total < 0) return 1;
    // A required list's values are each needed: the build stops here rather than the plugin at the game's start.
    for (size_t l = 0; l < list_count; ++l)
        if (lists[l].required && missing[l] > 0) {
            fprintf(stderr, "names table: %d required values of list %zu are not in this build (above)\n", missing[l], l);
            return 1;
        }
    return 0;
}

}  // namespace openrfg64
