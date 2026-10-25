#include "harness/rec_file.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rec {
namespace {

constexpr uint8_t kRecordTag = 0xA5;
constexpr size_t kWriteBlock = 64 * 1024;
constexpr uint32_t kWriteEveryFrames = 64;  // about a quarter second of gameplay
constexpr size_t kMinGap = 4;  // equal bytes that may be folded into a run instead of starting a new one

template <typename T>
void put(std::vector<uint8_t>& b, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}

bool write_all(HANDLE f, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(f, p, DWORD(size), &written, nullptr) || !written) return false;
        p += written;
        size -= written;
    }
    return true;
}

}  // namespace

// Record: u8 tag 0xA5, u8 flags, u8 end_state, u8 n_gov, f32 dt, f32 dt_real, u16 rng_index, u32 crt_rand,
//         i32 gov[n_gov], u16 n_runs, n_runs x (u16 offset, u16 length, u8 bytes[length]) = input bytes that
//         differ from the previous record's input (the first record is relative to all zeros).
bool Writer::open(const std::wstring& path, const Header& header) {
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    header_ = header;
    memset(prev_, 0, sizeof(prev_));
    buf_.clear();
    buf_.reserve(kWriteBlock * 2);
    since_write_ = 0;
    put(buf_, header_);
    committed_ = buf_.size();
    return true;
}

void Writer::append(const Frame& f) {
    if (!is_open()) return;
    put(buf_, kRecordTag);
    put(buf_, f.flags);
    put(buf_, f.end_state);
    const uint8_t n_gov = f.n_gov > kMaxGov ? uint8_t(kMaxGov) : f.n_gov;
    put(buf_, n_gov);
    put(buf_, f.dt);
    put(buf_, f.dt_real);
    put(buf_, f.rng_index);
    put(buf_, f.crt_rand);
    for (uint8_t i = 0; i < n_gov; ++i) put(buf_, f.gov[i]);

    const size_t count_at = buf_.size();
    put(buf_, uint16_t(0));
    uint16_t runs = 0;
    for (uint32_t i = 0; i < kInputSize;) {
        if (f.input[i] == prev_[i]) {
            ++i;
            continue;
        }
        uint32_t end = i + 1, last_diff = i;
        while (end < kInputSize && end - last_diff <= kMinGap) {
            if (f.input[end] != prev_[end]) last_diff = end;
            ++end;
        }
        const uint16_t len = uint16_t(last_diff + 1 - i);
        put(buf_, uint16_t(i));
        put(buf_, len);
        buf_.insert(buf_.end(), f.input + i, f.input + i + len);
        ++runs;
        i = last_diff + 1;
    }
    memcpy(buf_.data() + count_at, &runs, sizeof(runs));
    memcpy(prev_, f.input, kInputSize);
    committed_ = buf_.size();
    if (buf_.size() >= kWriteBlock || ++since_write_ >= kWriteEveryFrames) write_out();
}

void Writer::write_out() {
    write_all(file_, buf_.data(), committed_);
    buf_.erase(buf_.begin(), buf_.begin() + ptrdiff_t(committed_));
    committed_ = 0;
    since_write_ = 0;
}

void Writer::finish(uint32_t frame_count) {
    if (!is_open()) return;
    write_out();
    header_.frame_count = frame_count;
    SetFilePointer(file_, 0, nullptr, FILE_BEGIN);
    write_all(file_, &header_, sizeof(header_));
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}

void Writer::flush_at_exit(uint32_t frame_count) {
    if (!is_open()) return;
    finish(frame_count);
}

bool Reader::open(const std::wstring& path, std::string& error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "cannot open input.rfgrec";
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    data_.resize(size_t(size.QuadPart));
    DWORD got = 0;
    const bool ok = data_.empty() || (ReadFile(f, data_.data(), DWORD(data_.size()), &got, nullptr) && got == data_.size());
    CloseHandle(f);
    if (!ok || data_.size() < sizeof(Header)) {
        error = "input.rfgrec is truncated";
        return false;
    }
    memcpy(&header_, data_.data(), sizeof(Header));
    if (memcmp(header_.magic, "RFGREC01", 8) != 0 || header_.header_size != sizeof(Header) ||
        header_.input_size != kInputSize) {
        error = "input.rfgrec has an unknown format";
        return false;
    }
    pos_ = sizeof(Header);
    memset(prev_, 0, sizeof(prev_));
    return true;
}

bool Reader::next(Frame& f) {
    auto take = [&](void* out, size_t n) {
        if (pos_ + n > data_.size()) return false;
        memcpy(out, data_.data() + pos_, n);
        pos_ += n;
        return true;
    };
    uint8_t tag = 0;
    if (!take(&tag, 1) || tag != kRecordTag) return false;
    if (!take(&f.flags, 1) || !take(&f.end_state, 1) || !take(&f.n_gov, 1) || f.n_gov > kMaxGov) return false;
    if (!take(&f.dt, 4) || !take(&f.dt_real, 4) || !take(&f.rng_index, 2) || !take(&f.crt_rand, 4)) return false;
    for (uint8_t i = 0; i < f.n_gov; ++i)
        if (!take(&f.gov[i], 4)) return false;
    uint16_t runs = 0;
    if (!take(&runs, 2)) return false;
    for (uint16_t r = 0; r < runs; ++r) {
        uint16_t off = 0, len = 0;
        if (!take(&off, 2) || !take(&len, 2) || uint32_t(off) + len > kInputSize) return false;
        if (!take(prev_ + off, len)) return false;
    }
    memcpy(f.input, prev_, kInputSize);
    return true;
}

bool TextFile::open(const std::wstring& path) {
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
    buf_.clear();
    return file_ != INVALID_HANDLE_VALUE;
}

void TextFile::write(const std::string& s) {
    buf_ += s;
    if (buf_.size() > 16384) flush();
}

void TextFile::printf(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) write(std::string(line, size_t(n < int(sizeof(line)) ? n : int(sizeof(line)) - 1)));
}

void TextFile::flush() {
    if (file_ == INVALID_HANDLE_VALUE || buf_.empty()) return;
    write_all(file_, buf_.data(), buf_.size());
    buf_.clear();
}

void TextFile::close() {
    flush();
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}

bool read_text_file(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    const bool ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
    CloseHandle(f);
    return ok;
}

}  // namespace rec
