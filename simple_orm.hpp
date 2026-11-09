// simple_orm.hpp -- a small header-only ORM that stores tables as JSON lines.
//
//   * C++20, no external dependencies.
//   * One file per table: "<T::name()>.json", one JSON object per line.
//   * Objects can only be obtained from DBManager<T>::create() / search().
//
// Implementation notes (for readers of the source):
//   - Every table keeps an in-memory image of its file (lines + key index).  The
//     image is validated against the file's (size, mtime, inode) before it is
//     trusted, so files that are edited or deleted behind our back are re-read.
//   - Appends are O(1); replacing a line with one of the same length is patched
//     in place; any other replacement rewrites the file once.
//   - Objects are created through a private subclass (orm_detail::Instance<T>)
//     that overrides a pure virtual function of DBTableType.  Hence the user's
//     table type is abstract and `Student s;` / `new Student()` do not compile.
#ifndef SIMPLE_ORM_HPP
#define SIMPLE_ORM_HPP

#include <version>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if !defined(SIMPLE_ORM_PORTABLE_IO) && (defined(__unix__) || defined(__APPLE__))
#define SIMPLE_ORM_POSIX_IO 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#define SIMPLE_ORM_FP_CHARCONV 1
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class FieldBase;
class DBTableType;
template <class T> class DBManager;
template <class T> class ForeignKey;
template <class T> class ForeignKeyVector;
template <class T> class Result;

namespace orm_detail {
struct Access;
struct TableBase;
template <class T> class Table;
}  // namespace orm_detail

// ===========================================================================
// orm_detail: values, JSON primitives and file helpers
// ===========================================================================
namespace orm_detail {

enum class FieldKind : unsigned char { Int, Float, String, Date, ForeignKey, ForeignKeyVector };

using Date = std::chrono::year_month_day;
using Value = std::variant<std::int64_t, double, std::string, Date>;

template <class>
inline constexpr bool always_false_v = false;

inline Date default_date() noexcept {
    return Date{std::chrono::year{1970}, std::chrono::January, std::chrono::day{1}};
}

// ---- JSON writing ----------------------------------------------------------

inline void append_json_string(std::string& out, std::string_view s) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t run = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        out.append(s.data() + run, i - run);
        run = i + 1;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
        }
    }
    out.append(s.data() + run, s.size() - run);
    out.push_back('"');
}

inline void append_int(std::string& out, std::int64_t v) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

inline void append_double(std::string& out, double v) {
    if (!std::isfinite(v)) {  // JSON has no NaN/Infinity; keep the file valid.
        out += "null";
        return;
    }
#ifdef SIMPLE_ORM_FP_CHARCONV
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
#else
    char buf[40];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    out += buf;
#endif
}

inline void append_date(std::string& out, const Date& d) {
    char buf[40];
    const int n = std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", static_cast<int>(d.year()),
                                static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
    out.append(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}

inline void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ---- JSON reading ----------------------------------------------------------

class Reader {
public:
    explicit Reader(std::string_view s) noexcept : p_(s.data()), e_(s.data() + s.size()) {}

    [[noreturn]] static void fail() { throw std::runtime_error("simple_orm: malformed JSON"); }

    void skip_ws() noexcept {
        while (p_ != e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    char peek() noexcept {
        skip_ws();
        return p_ != e_ ? *p_ : '\0';
    }
    bool eat(char c) noexcept {
        skip_ws();
        if (p_ != e_ && *p_ == c) {
            ++p_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!eat(c)) fail();
    }
    bool eat_null() noexcept {
        skip_ws();
        if (e_ - p_ >= 4 && std::memcmp(p_, "null", 4) == 0) {
            p_ += 4;
            return true;
        }
        return false;
    }

    // Reads a JSON string (unescaped) into `out`, replacing its contents.
    void read_string(std::string& out) {
        out.clear();
        expect('"');
        for (;;) {
            const char* s = p_;
            while (p_ != e_ && *p_ != '"' && *p_ != '\\') ++p_;
            out.append(s, p_);
            if (p_ == e_) fail();
            if (*p_ == '"') {
                ++p_;
                return;
            }
            ++p_;  // backslash
            if (p_ == e_) fail();
            const char c = *p_++;
            switch (c) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        const char* save = p_;
                        p_ += 2;
                        const unsigned lo = hex4();
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p_ = save;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail();
            }
        }
    }

    // A number token such as -12, 3.5 or 1e+20.
    std::string_view number_token() {
        skip_ws();
        const char* s = p_;
        while (p_ != e_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '-' || *p_ == '+' || *p_ == '.' ||
                            *p_ == 'e' || *p_ == 'E'))
            ++p_;
        if (s == p_) fail();
        return std::string_view(s, static_cast<std::size_t>(p_ - s));
    }

    // The raw text of a scalar: a quoted string (with quotes and escapes) or a number.
    std::string_view raw_scalar() {
        skip_ws();
        if (p_ != e_ && *p_ == '"') {
            const char* s = p_++;
            while (p_ != e_ && *p_ != '"') {
                if (*p_ == '\\') {
                    ++p_;
                    if (p_ == e_) fail();
                }
                ++p_;
            }
            if (p_ == e_) fail();
            ++p_;
            return std::string_view(s, static_cast<std::size_t>(p_ - s));
        }
        return number_token();
    }

    // Skips any JSON value (used for unknown fields).
    void skip_value() {
        skip_ws();
        if (p_ == e_) fail();
        if (*p_ == '"') {
            raw_scalar();
        } else if (*p_ == '{' || *p_ == '[') {
            int depth = 0;
            while (p_ != e_) {
                const char c = *p_;
                if (c == '"') {
                    raw_scalar();
                    continue;
                }
                ++p_;
                if (c == '{' || c == '[') ++depth;
                if (c == '}' || c == ']') {
                    if (--depth == 0) return;
                }
            }
            fail();
        } else {
            while (p_ != e_ && *p_ != ',' && *p_ != '}' && *p_ != ']' && *p_ != ' ') ++p_;
        }
    }

private:
    unsigned hex4() {
        if (e_ - p_ < 4) fail();
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail();
        }
        return v;
    }

    const char* p_;
    const char* e_;
};

inline std::int64_t parse_int(std::string_view tok) {
    std::int64_t v{};
    const char* end = tok.data() + tok.size();
    auto r = std::from_chars(tok.data(), end, v);
    if (r.ec == std::errc{} && r.ptr == end) return v;
    // Tolerate "3.0" / "1e3" written by other tools.
    std::string tmp(tok);
    char* e = nullptr;
    const double d = std::strtod(tmp.c_str(), &e);
    if (e == tmp.c_str() + tmp.size()) return static_cast<std::int64_t>(d);
    Reader::fail();
}

inline double parse_double(std::string_view tok) {
#ifdef SIMPLE_ORM_FP_CHARCONV
    double v{};
    const char* end = tok.data() + tok.size();
    auto r = std::from_chars(tok.data(), end, v);
    if (r.ec == std::errc{} && r.ptr == end) return v;
    if (r.ec == std::errc::result_out_of_range && r.ptr == end) {
        std::string tmp(tok);
        return std::strtod(tmp.c_str(), nullptr);
    }
    Reader::fail();
#else
    std::string tmp(tok);
    char* e = nullptr;
    const double d = std::strtod(tmp.c_str(), &e);
    if (e != tmp.c_str() + tmp.size()) Reader::fail();
    return d;
#endif
}

inline Date parse_date(std::string_view s) {
    // YYYY-MM-DD (a leading '-' on the year is tolerated).
    std::size_t start = (!s.empty() && s[0] == '-') ? 1 : 0;
    const std::size_t d1 = s.find('-', start);
    if (d1 == std::string_view::npos) Reader::fail();
    const std::size_t d2 = s.find('-', d1 + 1);
    if (d2 == std::string_view::npos) Reader::fail();
    int y = 0;
    unsigned m = 0, d = 0;
    if (std::from_chars(s.data(), s.data() + d1, y).ec != std::errc{}) Reader::fail();
    if (std::from_chars(s.data() + d1 + 1, s.data() + d2, m).ec != std::errc{}) Reader::fail();
    if (std::from_chars(s.data() + d2 + 1, s.data() + s.size(), d).ec != std::errc{}) Reader::fail();
    return Date{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
}

// Canonical form of a key: decimal text for integers, the raw text for strings.
// Within one table the primary key has a single type, so this is unambiguous.
inline std::string canonical_from_json(std::string_view raw) {
    if (!raw.empty() && raw.front() == '"') {
        Reader r(raw);
        std::string s;
        r.read_string(s);
        return s;
    }
    return std::string(raw);
}

// ---- file helpers ----------------------------------------------------------

struct FileSig {
    bool exists = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::uint64_t ino = 0;
    friend bool operator==(const FileSig&, const FileSig&) = default;
};

namespace io {

[[noreturn]] inline void io_error(const std::string& what, const std::string& path) {
    throw std::runtime_error("simple_orm: cannot " + what + " '" + path + "'");
}

#ifdef SIMPLE_ORM_POSIX_IO

inline FileSig sig_from(const struct stat& st) {
    FileSig s;
    s.exists = true;
    s.size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
    s.mtime = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    s.mtime = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    s.ino = static_cast<std::uint64_t>(st.st_ino);
    return s;
}

inline FileSig stat_file(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return FileSig{};
    return sig_from(st);
}

inline void write_fd(int fd, const char* data, std::size_t n, const std::string& path) {
    std::size_t done = 0;
    while (done < n) {
        const ssize_t w = ::write(fd, data + done, n - done);
        if (w < 0) {
            ::close(fd);
            io_error("write", path);
        }
        done += static_cast<std::size_t>(w);
    }
}

inline FileSig append_file(const std::string& path, const std::string& data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) io_error("open", path);
    write_fd(fd, data.data(), data.size(), path);
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        io_error("stat", path);
    }
    ::close(fd);
    return sig_from(st);
}

// Overwrites bytes in place (every patch has exactly the length of the text it replaces).
inline FileSig patch_file(const std::string& path,
                          const std::vector<std::pair<std::uint64_t, const std::string*>>& patches) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) io_error("open", path);
    for (const auto& [off, text] : patches) {
        std::size_t done = 0;
        while (done < text->size()) {
            const ssize_t w = ::pwrite(fd, text->data() + done, text->size() - done,
                                       static_cast<off_t>(off + done));
            if (w < 0) {
                ::close(fd);
                io_error("write", path);
            }
            done += static_cast<std::size_t>(w);
        }
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        io_error("stat", path);
    }
    ::close(fd);
    return sig_from(st);
}

// Rewrites the file from `offset` on (everything before is unchanged) and cuts it to `total` bytes.
inline FileSig write_tail(const std::string& path, std::uint64_t offset, std::string_view data,
                          std::uint64_t total) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) io_error("open", path);
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::pwrite(fd, data.data() + done, data.size() - done,
                                   static_cast<off_t>(offset + done));
        if (w < 0) {
            ::close(fd);
            io_error("write", path);
        }
        done += static_cast<std::size_t>(w);
    }
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
        ::close(fd);
        io_error("truncate", path);
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        io_error("stat", path);
    }
    ::close(fd);
    return sig_from(st);
}

#else  // portable fallback

inline FileSig stat_file(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return FileSig{};
    FileSig s;
    s.exists = true;
    s.size = static_cast<std::uint64_t>(size);
    const auto t = std::filesystem::last_write_time(path, ec);
    if (!ec) s.mtime = static_cast<std::int64_t>(t.time_since_epoch().count());
    return s;
}

inline FileSig append_file(const std::string& path, const std::string& data) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) io_error("open", path);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) io_error("write", path);
    }
    return stat_file(path);
}

inline FileSig patch_file(const std::string& path,
                          const std::vector<std::pair<std::uint64_t, const std::string*>>& patches) {
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) io_error("open", path);
        for (const auto& [off, text] : patches) {
            f.seekp(static_cast<std::streamoff>(off));
            f.write(text->data(), static_cast<std::streamsize>(text->size()));
        }
        f.flush();
        if (!f) io_error("write", path);
    }
    return stat_file(path);
}

inline FileSig write_tail(const std::string& path, std::uint64_t offset, std::string_view data,
                          std::uint64_t total) {
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) io_error("open", path);
        f.seekp(static_cast<std::streamoff>(offset));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) io_error("write", path);
    }
    std::error_code ec;
    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(total), ec);
    if (ec) io_error("truncate", path);
    return stat_file(path);
}

#endif

inline bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    in.seekg(0, std::ios::beg);
    if (n < 0) return false;
    out.resize(static_cast<std::size_t>(n));
    in.read(out.data(), static_cast<std::streamsize>(n));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return true;
}

inline FileSig write_file(const std::string& path, const std::string& data) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) io_error("open", path);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) io_error("write", path);
    }
    return stat_file(path);
}

}  // namespace io

// While > 0, DBManager<T>::get_next_key() does not hand out keys.  Used while
// objects are built for the loader / the prototype, so that searching never
// uses up a key.
inline int& key_suppress_depth() noexcept {
    static int depth = 0;
    return depth;
}
struct KeySuppress {
    KeySuppress() noexcept { ++key_suppress_depth(); }
    ~KeySuppress() { --key_suppress_depth(); }
    KeySuppress(const KeySuppress&) = delete;
    KeySuppress& operator=(const KeySuppress&) = delete;
};

}  // namespace orm_detail

// ===========================================================================
// Fields
// ===========================================================================
class FieldBase {
public:
    FieldBase(const FieldBase& other) : name_(other.name_) {}  // owner is not copied
    FieldBase& operator=(const FieldBase&) noexcept { return *this; }
    virtual ~FieldBase() = default;

    const std::string& name() const noexcept { return name_; }

protected:
    explicit FieldBase(std::string name) : name_(std::move(name)) {}
    void touch();  // tells the owning object that this field was assigned

private:
    friend struct orm_detail::Access;

    virtual orm_detail::FieldKind kind_() const noexcept = 0;
    virtual void write_json_(std::string& out) const = 0;
    virtual void read_json_(orm_detail::Reader& r) = 0;
    virtual void append_key_(std::string& out) const = 0;
    virtual int compare_value_(const orm_detail::Value& v) const = 0;
    virtual int compare_field_(const FieldBase& other) const = 0;

    std::string name_;
    DBTableType* owner_ = nullptr;
};

namespace orm_detail {

template <class V>
struct ScalarTraits;
template <>
struct ScalarTraits<std::int64_t> {
    static constexpr FieldKind kind = FieldKind::Int;
    static std::int64_t default_value() noexcept { return 0; }
};
template <>
struct ScalarTraits<double> {
    static constexpr FieldKind kind = FieldKind::Float;
    static double default_value() noexcept { return 0.0; }
};
template <>
struct ScalarTraits<std::string> {
    static constexpr FieldKind kind = FieldKind::String;
    static std::string default_value() { return {}; }
};
template <>
struct ScalarTraits<Date> {
    static constexpr FieldKind kind = FieldKind::Date;
    static Date default_value() noexcept { return default_date(); }
};

template <class A, class B>
constexpr int cmp3(const A& a, const B& b) noexcept(noexcept(a < b)) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

}  // namespace orm_detail

// A field holding one value of type V.
template <class V>
class Scalar final : public FieldBase {
public:
    using value_type = V;
    using get_type = std::conditional_t<std::is_same_v<V, std::string>, const std::string&, V>;

    explicit Scalar(std::string name)
        : FieldBase(std::move(name)), value_(orm_detail::ScalarTraits<V>::default_value()) {}
    Scalar(std::string name, V value) : FieldBase(std::move(name)), value_(std::move(value)) {}
    Scalar(const Scalar&) = default;

    Scalar& operator=(const V& v) {
        value_ = v;
        touch();
        return *this;
    }
    Scalar& operator=(const Scalar& other) {
        if (this != &other) {
            value_ = other.value_;
            touch();
        }
        return *this;
    }
    template <class U = V, class = std::enable_if_t<std::is_same_v<U, std::string>>>
    Scalar& operator=(std::string_view v) {
        value_.assign(v.data(), v.size());
        touch();
        return *this;
    }
    template <class U = V, class = std::enable_if_t<std::is_same_v<U, std::string>>>
    Scalar& operator=(const char* v) {
        value_.assign(v);
        touch();
        return *this;
    }

    get_type get() const noexcept { return value_; }

private:
    orm_detail::FieldKind kind_() const noexcept override { return orm_detail::ScalarTraits<V>::kind; }

    void write_json_(std::string& out) const override {
        if constexpr (std::is_same_v<V, std::int64_t>) {
            orm_detail::append_int(out, value_);
        } else if constexpr (std::is_same_v<V, double>) {
            orm_detail::append_double(out, value_);
        } else if constexpr (std::is_same_v<V, std::string>) {
            orm_detail::append_json_string(out, value_);
        } else {
            out.push_back('"');
            orm_detail::append_date(out, value_);
            out.push_back('"');
        }
    }

    void read_json_(orm_detail::Reader& r) override {
        if constexpr (std::is_same_v<V, std::int64_t>) {
            value_ = r.eat_null() ? 0 : orm_detail::parse_int(r.number_token());
        } else if constexpr (std::is_same_v<V, double>) {
            value_ = r.eat_null() ? 0.0 : orm_detail::parse_double(r.number_token());
        } else if constexpr (std::is_same_v<V, std::string>) {
            if (r.eat_null()) value_.clear();
            else r.read_string(value_);
        } else {
            if (r.eat_null()) {
                value_ = orm_detail::default_date();
            } else {
                std::string tmp;
                r.read_string(tmp);
                value_ = orm_detail::parse_date(tmp);
            }
        }
    }

    void append_key_(std::string& out) const override {
        if constexpr (std::is_same_v<V, std::int64_t>) {
            orm_detail::append_int(out, value_);
        } else if constexpr (std::is_same_v<V, std::string>) {
            out += value_;
        } else {
            write_json_(out);
        }
    }

    int compare_value_(const orm_detail::Value& v) const override {
        if constexpr (std::is_same_v<V, std::int64_t>) {
            return orm_detail::cmp3(value_, std::get<0>(v));
        } else if constexpr (std::is_same_v<V, double>) {
            const double o = v.index() == 0 ? static_cast<double>(std::get<0>(v)) : std::get<1>(v);
            return orm_detail::cmp3(value_, o);
        } else if constexpr (std::is_same_v<V, std::string>) {
            const int c = value_.compare(std::get<2>(v));
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        } else {
            return orm_detail::cmp3(value_, std::get<3>(v));
        }
    }

    int compare_field_(const FieldBase& other) const override {
        return orm_detail::cmp3(value_, static_cast<const Scalar&>(other).value_);
    }

    V value_;
};

using IntType = Scalar<std::int64_t>;
using FloatType = Scalar<double>;
using StringType = Scalar<std::string>;
using DateType = Scalar<std::chrono::year_month_day>;

template <class V>
std::ostream& operator<<(std::ostream& os, const Scalar<V>& field) {
    if constexpr (std::is_same_v<V, std::chrono::year_month_day>) {
        std::string s;
        orm_detail::append_date(s, field.get());
        return os << s;
    } else {
        return os << field.get();
    }
}

// The primary key of a table: an IntType or a StringType.
class PrimaryKey {
public:
    explicit PrimaryKey(IntType* field) noexcept : field_(field) {}
    explicit PrimaryKey(StringType* field) noexcept : field_(field) {}
    FieldBase* field() const noexcept { return field_; }

private:
    FieldBase* field_;
};

// ===========================================================================
// DBTableType
// ===========================================================================
namespace orm_detail {

struct TableBase {
    virtual ~TableBase() = default;
    virtual void save_object(DBTableType& obj) = 0;
    virtual void key_changed(DBTableType& obj) = 0;
};

struct ObjectState {
    enum class Idx : unsigned char { None, Pending, Claimed };

    std::vector<FieldBase*> fields;  // in declaration order, primary key first
    FieldBase* pk = nullptr;
    TableBase* table = nullptr;
    std::string idx_key;  // key under which the object is indexed (valid unless idx == None)
    Idx idx = Idx::None;  // Claimed: saved or loaded.  Pending: created, not yet saved.
    bool modified = true;
    bool in_dirty = false;
    bool has_fk = false;
};

}  // namespace orm_detail

class DBTableType {
public:
    DBTableType(const DBTableType&) = delete;
    DBTableType& operator=(const DBTableType&) = delete;
    virtual ~DBTableType() = default;

    // Saves this object to its table's JSON file.
    void save() {
        if (st_.table == nullptr) throw std::logic_error("simple_orm: object does not belong to a table");
        st_.table->save_object(*this);
    }

protected:
    template <class... Fs>
    explicit DBTableType(PrimaryKey pk, Fs*... fields) {
        static_assert((std::is_base_of_v<FieldBase, Fs> && ...),
                      "DBTableType: every field must be an IntType, FloatType, StringType, DateType, "
                      "ForeignKey<T> or ForeignKeyVector<T>");
        // The fields are not constructed yet (they are members of the derived class),
        // so only their addresses are stored here.
        st_.fields.reserve(sizeof...(Fs) + 1);
        st_.pk = pk.field();
        st_.fields.push_back(pk.field());
        (st_.fields.push_back(fields), ...);
    }

    // Pure virtual on purpose: user tables are abstract, so they cannot be created
    // directly.  DBManager<T>::create() instantiates a private subclass that overrides it.
    virtual void simple_orm_create_only_() = 0;

private:
    friend struct orm_detail::Access;
    orm_detail::ObjectState st_;
};

namespace orm_detail {

struct Access {
    static ObjectState& st(DBTableType& t) noexcept { return t.st_; }

    static const std::string& name(const FieldBase& f) noexcept { return f.name_; }
    static FieldKind kind(const FieldBase& f) noexcept { return f.kind_(); }
    static void write_json(const FieldBase& f, std::string& out) { f.write_json_(out); }
    static void read_json(FieldBase& f, Reader& r) { f.read_json_(r); }
    static void append_key(const FieldBase& f, std::string& out) { f.append_key_(out); }
    static int compare_value(const FieldBase& f, const Value& v) { return f.compare_value_(v); }
    static int compare_field(const FieldBase& a, const FieldBase& b) { return a.compare_field_(b); }

    static void pk_json(DBTableType& t, std::string& out) { t.st_.pk->write_json_(out); }

    // Connects the fields to their object and the object to its table.
    static void bind(DBTableType& t, TableBase* table) {
        t.st_.table = table;
        for (FieldBase* f : t.st_.fields) {
            f->owner_ = &t;
            const FieldKind k = f->kind_();
            if (k == FieldKind::ForeignKey || k == FieldKind::ForeignKeyVector) t.st_.has_fk = true;
        }
    }

    static void touched(FieldBase& f) {
        DBTableType& owner = *f.owner_;
        ObjectState& s = owner.st_;
        s.modified = true;
        if (&f == s.pk && s.table != nullptr) s.table->key_changed(owner);
    }
};

}  // namespace orm_detail

inline void FieldBase::touch() {
    if (owner_ != nullptr) orm_detail::Access::touched(*this);
}

// ===========================================================================
// Foreign keys
// ===========================================================================
// A reference to at most one object of table T.  T may still be incomplete where
// the member is declared; it must be complete where get() / operator= are used.
template <class T>
class ForeignKey final : public FieldBase {
public:
    explicit ForeignKey(std::string name) : FieldBase(std::move(name)) {}
    ForeignKey(const ForeignKey&) = default;

    ForeignKey& operator=(T* target) {
        set_target(target);
        return *this;
    }
    ForeignKey& operator=(std::nullptr_t) {
        set_target(nullptr);
        return *this;
    }
    ForeignKey& operator=(const ForeignKey& other) {
        if (this != &other) {
            target_ = other.target_;
            key_json_ = other.key_json_;
            has_key_ = other.has_key_;
            touch();
        }
        return *this;
    }

    // The referenced object, or nullptr if there is none or its key no longer exists.
    T* get() const {
        if (target_ != nullptr) return static_cast<T*>(target_);
        if (!has_key_) return nullptr;
        T* found = DBManager<T>::find_by_json_(key_json_);
        if (found != nullptr) target_ = found;
        return found;
    }

private:
    void set_target(T* target) {
        target_ = target;
        key_json_.clear();
        has_key_ = false;
        touch();
    }

    orm_detail::FieldKind kind_() const noexcept override { return orm_detail::FieldKind::ForeignKey; }
    void write_json_(std::string& out) const override {
        if (target_ != nullptr) orm_detail::Access::pk_json(*target_, out);
        else if (has_key_) out += key_json_;
        else out += "null";
    }
    void read_json_(orm_detail::Reader& r) override {
        target_ = nullptr;
        if (r.eat_null()) {
            has_key_ = false;
            key_json_.clear();
        } else {
            key_json_.assign(r.raw_scalar());
            has_key_ = true;
        }
    }
    void append_key_(std::string&) const override {}
    int compare_value_(const orm_detail::Value&) const override { return 0; }
    int compare_field_(const FieldBase&) const override { return 0; }

    mutable DBTableType* target_ = nullptr;
    std::string key_json_;  // raw JSON text of the key while the object is not resolved
    bool has_key_ = false;
};

// References to any number of objects of table T.
template <class T>
class ForeignKeyVector final : public FieldBase {
public:
    explicit ForeignKeyVector(std::string name) : FieldBase(std::move(name)) {}
    ForeignKeyVector(const ForeignKeyVector&) = default;

    ForeignKeyVector& operator=(const ForeignKeyVector& other) {
        if (this != &other) {
            refs_ = other.refs_;
            touch();
        }
        return *this;
    }

    // A null pointer (e.g. the result of a failed search) is ignored.
    void push_back(T* target) {
        if (target == nullptr) return;
        refs_.push_back(Ref{target, {}});
        touch();
    }
    void clear() {
        refs_.clear();
        touch();
    }
    std::size_t size() const noexcept { return refs_.size(); }
    bool empty() const noexcept { return refs_.empty(); }

    // The referenced objects in insertion order; keys whose object does not exist are skipped.
    std::vector<T*> get() const {
        std::vector<T*> out;
        out.reserve(refs_.size());
        for (Ref& ref : refs_) {
            if (ref.obj == nullptr) {
                T* found = DBManager<T>::find_by_json_(ref.key_json);
                if (found == nullptr) continue;
                ref.obj = found;
            }
            out.push_back(static_cast<T*>(ref.obj));
        }
        return out;
    }

private:
    struct Ref {
        DBTableType* obj = nullptr;
        std::string key_json;  // raw JSON text of the key while obj is null
    };

    orm_detail::FieldKind kind_() const noexcept override { return orm_detail::FieldKind::ForeignKeyVector; }
    void write_json_(std::string& out) const override {
        out.push_back('[');
        bool first = true;
        for (const Ref& ref : refs_) {
            if (!first) out += ", ";
            first = false;
            if (ref.obj != nullptr) orm_detail::Access::pk_json(*ref.obj, out);
            else out += ref.key_json;
        }
        out.push_back(']');
    }
    void read_json_(orm_detail::Reader& r) override {
        refs_.clear();
        if (r.eat_null()) return;
        r.expect('[');
        if (r.eat(']')) return;
        for (;;) {
            Ref ref;
            ref.key_json.assign(r.raw_scalar());
            refs_.push_back(std::move(ref));
            if (r.eat(',')) continue;
            r.expect(']');
            break;
        }
    }
    void append_key_(std::string&) const override {}
    int compare_value_(const orm_detail::Value&) const override { return 0; }
    int compare_field_(const FieldBase&) const override { return 0; }

    mutable std::vector<Ref> refs_;
};

// ===========================================================================
// Search conditions, Ordering, Result
// ===========================================================================
enum class Ordering { Ascending, Descending };

namespace orm_detail {

struct CondNode {
    enum class Op { Eq, Gt, Lt, Ge, Le, And, Or, Not };
    Op op;
    Value value;
    std::shared_ptr<const CondNode> lhs;
    std::shared_ptr<const CondNode> rhs;
};

class Condition {
public:
    explicit Condition(std::shared_ptr<const CondNode> node) : node_(std::move(node)) {}
    const CondNode& node() const noexcept { return *node_; }
    const std::shared_ptr<const CondNode>& ptr() const noexcept { return node_; }

private:
    std::shared_ptr<const CondNode> node_;
};

template <class V>
Value to_value(const V& v) {
    using D = std::remove_cv_t<std::remove_reference_t<V>>;
    if constexpr (std::is_same_v<D, Date>) {
        return Value(std::in_place_index<3>, v);
    } else if constexpr (std::is_integral_v<D>) {
        return Value(std::in_place_index<0>, static_cast<std::int64_t>(v));
    } else if constexpr (std::is_floating_point_v<D>) {
        return Value(std::in_place_index<1>, static_cast<double>(v));
    } else if constexpr (std::is_convertible_v<const V&, std::string_view>) {
        return Value(std::in_place_index<2>, std::string(std::string_view(v)));
    } else {
        static_assert(always_false_v<V>,
                      "search condition values must be integers, floating point numbers, strings or "
                      "std::chrono::year_month_day");
    }
}

template <class V>
Condition make_cmp(CondNode::Op op, const V& v) {
    return Condition(std::make_shared<const CondNode>(CondNode{op, to_value(v), nullptr, nullptr}));
}

inline bool accepts(FieldKind kind, const Value& v) noexcept {
    switch (kind) {
        case FieldKind::Int: return v.index() == 0;
        case FieldKind::Float: return v.index() == 0 || v.index() == 1;
        case FieldKind::String: return v.index() == 2;
        case FieldKind::Date: return v.index() == 3;
        default: return false;
    }
}

inline void validate_cond(const CondNode& n, FieldKind kind, std::string_view field) {
    switch (n.op) {
        case CondNode::Op::And:
        case CondNode::Op::Or:
            validate_cond(*n.lhs, kind, field);
            validate_cond(*n.rhs, kind, field);
            return;
        case CondNode::Op::Not:
            validate_cond(*n.lhs, kind, field);
            return;
        default:
            if (!accepts(kind, n.value))
                throw std::invalid_argument("simple_orm: the value does not match the type of field '" +
                                            std::string(field) + "'");
    }
}

inline bool eval_cond(const CondNode& n, const FieldBase& f) {
    using Op = CondNode::Op;
    switch (n.op) {
        case Op::And: return eval_cond(*n.lhs, f) && eval_cond(*n.rhs, f);
        case Op::Or: return eval_cond(*n.lhs, f) || eval_cond(*n.rhs, f);
        case Op::Not: return !eval_cond(*n.lhs, f);
        default: break;
    }
    const int c = Access::compare_value(f, n.value);
    switch (n.op) {
        case Op::Eq: return c == 0;
        case Op::Gt: return c > 0;
        case Op::Lt: return c < 0;
        case Op::Ge: return c >= 0;
        case Op::Le: return c <= 0;
        default: return false;
    }
}

}  // namespace orm_detail

template <class V>
orm_detail::Condition EQUAL_TO(const V& v) {
    return orm_detail::make_cmp(orm_detail::CondNode::Op::Eq, v);
}
template <class V>
orm_detail::Condition GREATER_THAN(const V& v) {
    return orm_detail::make_cmp(orm_detail::CondNode::Op::Gt, v);
}
template <class V>
orm_detail::Condition LESS_THAN(const V& v) {
    return orm_detail::make_cmp(orm_detail::CondNode::Op::Lt, v);
}
template <class V>
orm_detail::Condition GREATER_THAN_OR_EQUAL_TO(const V& v) {
    return orm_detail::make_cmp(orm_detail::CondNode::Op::Ge, v);
}
template <class V>
orm_detail::Condition LESS_THAN_OR_EQUAL_TO(const V& v) {
    return orm_detail::make_cmp(orm_detail::CondNode::Op::Le, v);
}
inline orm_detail::Condition AND(const orm_detail::Condition& a, const orm_detail::Condition& b) {
    return orm_detail::Condition(std::make_shared<const orm_detail::CondNode>(
        orm_detail::CondNode{orm_detail::CondNode::Op::And, orm_detail::Value{}, a.ptr(), b.ptr()}));
}
inline orm_detail::Condition OR(const orm_detail::Condition& a, const orm_detail::Condition& b) {
    return orm_detail::Condition(std::make_shared<const orm_detail::CondNode>(
        orm_detail::CondNode{orm_detail::CondNode::Op::Or, orm_detail::Value{}, a.ptr(), b.ptr()}));
}
inline orm_detail::Condition NOT(const orm_detail::Condition& a) {
    return orm_detail::Condition(std::make_shared<const orm_detail::CondNode>(
        orm_detail::CondNode{orm_detail::CondNode::Op::Not, orm_detail::Value{}, a.ptr(), nullptr}));
}

// The objects found by an advanced search.  The pointers are the same objects that
// DBManager<T>::search(key) returns.
template <class T>
class Result {
public:
    using value_type = T*;
    using const_iterator = typename std::vector<T*>::const_iterator;
    using iterator = const_iterator;

    Result() = default;
    explicit Result(std::vector<T*> items) : items_(std::move(items)) {}

    std::size_t size() const noexcept { return items_.size(); }
    bool empty() const noexcept { return items_.empty(); }
    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    T* operator[](std::size_t i) const { return items_[i]; }

    // Returns the objects sorted by `field`; equal values are ordered by primary key (ascending).
    Result order_by(std::string_view field, Ordering ordering = Ordering::Ascending) const {
        Result sorted(*this);
        DBManager<T>::sort_(sorted.items_, field, ordering);
        return sorted;
    }

private:
    std::vector<T*> items_;
};

// ===========================================================================
// Table<T>: per-table state (objects in memory + image of the JSON file)
// ===========================================================================
namespace orm_detail {

// The class DBManager<T>::create() actually instantiates.  It overrides the pure
// virtual function of DBTableType, which the user's own type does not.
template <class T>
struct Instance final : T {
    Instance() : T() {}

private:
    void simple_orm_create_only_() override {}
};

// The text of a table's file as we believe it to be, kept in one buffer.
struct FileImage {
    bool valid = false;
    FileSig sig;                                   // what the file looked like when we last saw it
    std::string content;                           // every line followed by '\n'
    std::vector<std::uint64_t> offsets{0};         // start of each line; back() == content.size()
    std::vector<std::string> keys;                 // canonical key of each line
    std::unordered_map<std::string, std::size_t> index;  // key -> line number
    bool disk_ok = true;                           // the file on disk is exactly `content`
    bool has_max = false;                          // largest integer key in the file
    std::int64_t max_int = 0;

    std::size_t count() const noexcept { return keys.size(); }
    std::string_view line(std::size_t i) const noexcept {
        return std::string_view(content.data() + offsets[i],
                                static_cast<std::size_t>(offsets[i + 1] - offsets[i] - 1));
    }
};

inline std::int64_t saturating_inc(std::int64_t v) noexcept {
    return v == INT64_MAX ? v : v + 1;
}

template <class T>
class Table final : public TableBase {
    using Idx = ObjectState::Idx;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    struct Change {
        T* obj;
        std::size_t idx;  // line to replace (unless append)
        bool append;
        std::string key;
        std::string line;
    };

public:
    Table() : filename_(std::string(T::name()) + ".json") {}
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    // ---- creation ---------------------------------------------------------
    T* create() {
        auto up = std::make_unique<Instance<T>>();
        T* obj = up.get();
        Access::bind(*obj, this);
        owned_.push_back(std::move(up));
        objs_.push_back(obj);
        index_pending(obj);
        return obj;
    }

    // ---- keys -------------------------------------------------------------
    FieldKind pk_kind() { return Access::kind(*Access::st(proto()).pk); }

    std::int64_t next_key() {
        sync();
        std::int64_t next = image_.has_max ? saturating_inc(image_.max_int) : 1;
        if (has_issued_ && last_issued_ >= next) next = saturating_inc(last_issued_);
        has_issued_ = true;
        last_issued_ = next;
        return next;
    }

    // ---- lookup -----------------------------------------------------------
    T* find(const std::string& canon) {
        process_dirty();
        if (auto it = claimed_.find(canon); it != claimed_.end()) return it->second;
        if (auto it = pending_.find(canon); it != pending_.end()) return it->second;
        sync();
        auto it = image_.index.find(canon);
        if (it == image_.index.end()) return nullptr;
        return load_line(it->second);
    }

    void load_all() {
        sync();
        if (all_loaded_) return;
        process_dirty();
        for (std::size_t i = 0; i < image_.count(); ++i) {
            const std::string& k = image_.keys[i];
            if (claimed_.find(k) != claimed_.end() || pending_.find(k) != pending_.end()) continue;
            load_line(i);
        }
        all_loaded_ = true;
    }

    // ---- saving -----------------------------------------------------------
    void save_all() { save_span(std::span<T* const>(objs_.data(), objs_.size())); }

    void save_object(DBTableType& base) override {
        T* obj = static_cast<T*>(&base);
        save_span(std::span<T* const>(&obj, 1));
    }

    void key_changed(DBTableType& base) override {
        ObjectState& st = Access::st(base);
        if (st.idx == Idx::Claimed || st.in_dirty) return;
        st.in_dirty = true;
        dirty_.push_back(static_cast<T*>(&base));
    }

    // ---- advanced search --------------------------------------------------
    Result<T> search_where(std::string_view field, const Condition& cond) {
        const std::size_t idx = field_index(field);
        const FieldKind kind = Access::kind(*Access::st(proto()).fields[idx]);
        validate_cond(cond.node(), kind, field);
        load_all();
        process_dirty();
        std::vector<T*> out;
        const CondNode& node = cond.node();
        for (T* obj : objs_) {
            if (eval_cond(node, *Access::st(*obj).fields[idx])) out.push_back(obj);
        }
        return Result<T>(std::move(out));
    }

    void sort(std::vector<T*>& items, std::string_view field, Ordering ordering) {
        const std::size_t idx = field_index(field);
        const bool asc = ordering == Ordering::Ascending;
        std::sort(items.begin(), items.end(), [&](T* a, T* b) {
            ObjectState& sa = Access::st(*a);
            ObjectState& sb = Access::st(*b);
            const int c = Access::compare_field(*sa.fields[idx], *sb.fields[idx]);
            if (c != 0) return asc ? c < 0 : c > 0;
            return Access::compare_field(*sa.pk, *sb.pk) < 0;
        });
    }

private:
    // ---- prototype (never exposed): knows the field names and kinds ----------
    Instance<T>& proto() {
        if (!proto_) {
            KeySuppress guard;
            proto_ = std::make_unique<Instance<T>>();
        }
        return *proto_;
    }

    std::size_t field_index(std::string_view field) {
        ObjectState& ps = Access::st(proto());
        std::size_t idx = npos;
        for (std::size_t i = 0; i < ps.fields.size(); ++i) {
            if (Access::name(*ps.fields[i]) == field) {
                idx = i;
                break;
            }
        }
        if (idx == npos)
            throw std::invalid_argument("simple_orm: table '" + std::string(T::name()) +
                                        "' has no field '" + std::string(field) + "'");
        const FieldKind kind = Access::kind(*ps.fields[idx]);
        if (kind == FieldKind::ForeignKey || kind == FieldKind::ForeignKeyVector)
            throw std::invalid_argument("simple_orm: field '" + std::string(field) +
                                        "' is a foreign key and cannot be searched");
        return idx;
    }

    // Text written before each field: `{"a": `, `, "b": `, ...
    const std::vector<std::string>& prefixes() {
        if (prefixes_.empty()) {
            ObjectState& ps = Access::st(proto());
            for (std::size_t i = 0; i < ps.fields.size(); ++i) {
                std::string p = i == 0 ? "{" : ", ";
                append_json_string(p, Access::name(*ps.fields[i]));
                p += ": ";
                prefixes_.push_back(std::move(p));
            }
        }
        return prefixes_;
    }

    void serialize(T& obj, std::string& out) {
        const auto& pre = prefixes();
        ObjectState& st = Access::st(obj);
        for (std::size_t i = 0; i < st.fields.size(); ++i) {
            out += pre[i];
            Access::write_json(*st.fields[i], out);
        }
        out.push_back('}');
    }

    // ---- object <-> line ----------------------------------------------------
    void parse_line(T& obj, std::string_view line) {
        ObjectState& st = Access::st(obj);
        Reader r(line);
        r.expect('{');
        if (r.eat('}')) return;
        std::string name;
        std::size_t guess = 0;
        for (;;) {
            r.read_string(name);
            r.expect(':');
            FieldBase* f = nullptr;
            if (guess < st.fields.size() && Access::name(*st.fields[guess]) == name) {
                f = st.fields[guess++];
            } else {
                for (std::size_t i = 0; i < st.fields.size(); ++i) {
                    if (Access::name(*st.fields[i]) == name) {
                        f = st.fields[i];
                        guess = i + 1;
                        break;
                    }
                }
            }
            if (f != nullptr) Access::read_json(*f, r);
            else r.skip_value();
            if (r.eat(',')) continue;
            r.expect('}');
            break;
        }
    }

    T* load_line(std::size_t i) {
        KeySuppress guard;  // building the object must not use up keys
        auto up = std::make_unique<Instance<T>>();
        T* obj = up.get();
        Access::bind(*obj, this);
        parse_line(*obj, image_.line(i));
        owned_.push_back(std::move(up));
        objs_.push_back(obj);
        ObjectState& st = Access::st(*obj);
        st.idx_key = image_.keys[i];
        st.idx = Idx::Claimed;
        st.modified = false;
        claimed_[st.idx_key] = obj;
        return obj;
    }

    // ---- indexes of the objects in memory --------------------------------------
    void unindex_pending(T* obj) {
        ObjectState& st = Access::st(*obj);
        if (st.idx != Idx::Pending) return;
        auto it = pending_.find(st.idx_key);
        if (it != pending_.end() && it->second == obj) pending_.erase(it);
        st.idx = Idx::None;
    }

    void index_pending(T* obj) {
        unindex_pending(obj);
        ObjectState& st = Access::st(*obj);
        st.idx_key.clear();
        Access::append_key(*st.pk, st.idx_key);
        st.idx = Idx::Pending;
        pending_.try_emplace(st.idx_key, obj);
    }

    void process_dirty() {
        if (dirty_.empty()) return;
        std::vector<T*> work;
        work.swap(dirty_);
        for (T* obj : work) {
            ObjectState& st = Access::st(*obj);
            st.in_dirty = false;
            if (st.idx != Idx::Claimed) index_pending(obj);
        }
    }

    void mark_saved(T* obj, const std::string& key) {
        ObjectState& st = Access::st(*obj);
        if (st.idx == Idx::Claimed) {
            if (st.idx_key != key) {
                claimed_.erase(st.idx_key);
                st.idx_key = key;
                claimed_[key] = obj;
            }
        } else {
            unindex_pending(obj);
            st.idx_key = key;
            st.idx = Idx::Claimed;
            claimed_[key] = obj;
        }
        st.modified = false;
    }

    // ---- file image -----------------------------------------------------------
    // Re-reads the file if it is not the one we last saw.
    void sync() {
        const FileSig now = io::stat_file(filename_);
        if (image_.valid && now == image_.sig) return;
        rebuild_image();
    }

    // Finds the primary key of a line.  It is the first field in files written by us; fields
    // written in another order are tolerated.
    static bool extract_key(std::string_view line, const std::string& pk_name, std::string& key,
                            bool& numeric) {
        try {
            Reader r(line);
            r.expect('{');
            std::string name;
            for (;;) {
                r.read_string(name);
                r.expect(':');
                if (name == pk_name) {
                    if (r.peek() == '"') {
                        r.read_string(key);
                        numeric = false;
                    } else {
                        key.assign(r.number_token());
                        numeric = true;
                    }
                    return true;
                }
                r.skip_value();
                if (!r.eat(',')) return false;
            }
        } catch (const std::runtime_error&) {
            return false;
        }
    }

    // Records a line (already in image_.content, or about to be appended to it) in the index.
    void register_line(const std::string& key, std::size_t line_len, bool numeric) {
        if (numeric) {
            std::int64_t v{};
            const auto r = std::from_chars(key.data(), key.data() + key.size(), v);
            if (r.ec == std::errc{} && r.ptr == key.data() + key.size()) {
                if (!image_.has_max || v > image_.max_int) image_.max_int = v;
                image_.has_max = true;
            }
        }
        image_.index.emplace(key, image_.keys.size());
        image_.keys.push_back(key);
        image_.offsets.push_back(image_.offsets.back() + line_len + 1);
    }

    void rebuild_image() {
        std::string raw;
        image_ = FileImage{};
        image_.sig = io::stat_file(filename_);
        if (image_.sig.exists && !io::read_file(filename_, raw)) raw.clear();

        struct Span {
            std::size_t start, len;
        };
        std::vector<Span> spans;
        const std::string pk_name = Access::name(*Access::st(proto()).pk);
        const char* base = raw.data();
        const std::size_t n = raw.size();
        bool canonical = true;  // every line valid, unique, non-empty, '\n'-terminated
        std::size_t pos = 0;
        std::string key;
        image_.offsets.clear();
        while (pos < n) {
            const char* nl = static_cast<const char*>(std::memchr(base + pos, '\n', n - pos));
            const std::size_t end = nl != nullptr ? static_cast<std::size_t>(nl - base) : n;
            std::size_t len = end - pos;
            if (nl == nullptr) canonical = false;
            if (len > 0 && base[pos + len - 1] == '\r') {
                --len;
                canonical = false;
            }
            bool valid_line = false;
            if (len > 0) {
                bool numeric = false;
                valid_line = extract_key(std::string_view(base + pos, len), pk_name, key, numeric) &&
                             image_.index.find(key) == image_.index.end();
                if (valid_line) {
                    if (numeric) {
                        std::int64_t v{};
                        const auto r = std::from_chars(key.data(), key.data() + key.size(), v);
                        if (r.ec == std::errc{} && r.ptr == key.data() + key.size()) {
                            if (!image_.has_max || v > image_.max_int) image_.max_int = v;
                            image_.has_max = true;
                        }
                    }
                    image_.index.emplace(key, spans.size());
                    image_.keys.push_back(key);
                    spans.push_back(Span{pos, len});
                }
            }
            if (!valid_line) canonical = false;
            pos = nl != nullptr ? end + 1 : n;
        }

        if (canonical) {
            for (const Span& sp : spans) image_.offsets.push_back(sp.start);
            image_.content = std::move(raw);
        } else {
            // blank / malformed / duplicate lines: keep a clean copy; the file is rewritten on the next save
            image_.content.reserve(n);
            for (const Span& sp : spans) {
                image_.offsets.push_back(image_.content.size());
                image_.content.append(raw, sp.start, sp.len);
                image_.content.push_back('\n');
            }
            image_.disk_ok = false;
        }
        image_.offsets.push_back(image_.content.size());
        image_.valid = true;
        all_loaded_ = false;
        has_issued_ = false;  // the file changed behind our back: forget the keys we handed out
    }

    // ---- writing ------------------------------------------------------------------
    bool needs_save(T& obj) {
        const ObjectState& st = Access::st(obj);
        return !(st.idx == Idx::Claimed && !st.modified && !st.has_fk);
    }

    [[noreturn]] void duplicate_key(const std::string& key) {
        throw std::invalid_argument("simple_orm: another object of table '" + std::string(T::name()) +
                                    "' already has the key '" + key + "'");
    }

    void save_span(std::span<T* const> targets) {
        bool any = false;
        for (T* obj : targets) {
            if (needs_save(*obj)) {
                any = true;
                break;
            }
        }
        if (!any) return;

        sync();
        process_dirty();

        std::vector<Change> changes;
        std::vector<T*> clean;
        std::unordered_map<std::string, T*> batch;  // new keys of this call
        std::string key;
        for (T* obj : targets) {
            if (!needs_save(*obj)) continue;
            ObjectState& st = Access::st(*obj);
            key.clear();
            Access::append_key(*st.pk, key);
            const bool own = st.idx == Idx::Claimed && st.idx_key == key;

            auto line_it = image_.index.find(key);
            if (!own) {
                auto c = claimed_.find(key);
                if (c != claimed_.end() && c->second != obj) duplicate_key(key);
                if (line_it != image_.index.end()) duplicate_key(key);
                if (targets.size() > 1) {
                    auto [it, inserted] = batch.emplace(key, obj);
                    if (!inserted && it->second != obj) duplicate_key(key);
                }
            }

            std::string line;
            serialize(*obj, line);
            if (line_it != image_.index.end()) {
                if (image_.line(line_it->second) == line) clean.push_back(obj);
                else changes.push_back(Change{obj, line_it->second, false, key, std::move(line)});
            } else {
                changes.push_back(Change{obj, npos, true, key, std::move(line)});
            }
        }

        apply(changes);
        for (Change& c : changes) mark_saved(c.obj, c.key);
        for (T* obj : clean) Access::st(*obj).modified = false;
    }

    void apply(std::vector<Change>& changes) {
        if (changes.empty()) return;
        const bool int_pk = pk_kind() == FieldKind::Int;

        std::size_t replaced = 0;
        std::size_t first_replaced = npos;
        bool resized = false;
        for (const Change& c : changes) {
            if (c.append) continue;
            ++replaced;
            first_replaced = std::min(first_replaced, c.idx);
            if (c.line.size() != image_.line(c.idx).size()) resized = true;
        }

        try {
            if (image_.disk_ok && !resized && replaced <= 32) {
                // Same-length replacements are patched in place; new lines are appended.
                std::vector<std::pair<std::uint64_t, const std::string*>> patches;
                std::string tail;
                for (const Change& c : changes) {
                    if (c.append) {
                        tail += c.line;
                        tail.push_back('\n');
                    } else {
                        const std::uint64_t off = image_.offsets[c.idx];
                        image_.content.replace(static_cast<std::size_t>(off), c.line.size(), c.line);
                        patches.emplace_back(off, &c.line);
                    }
                }
                FileSig sig = image_.sig;
                if (!patches.empty()) sig = io::patch_file(filename_, patches);
                if (!tail.empty()) sig = io::append_file(filename_, tail);
                for (const Change& c : changes) {
                    if (!c.append) continue;
                    image_.content += c.line;
                    image_.content.push_back('\n');
                    register_line(c.key, c.line.size(), int_pk);
                }
                image_.sig = sig;
                return;
            }

            // Everything else rewrites the file from the first changed line on, in one write.
            std::stable_sort(changes.begin(), changes.end(),
                             [](const Change& a, const Change& b) { return a.idx < b.idx; });
            const std::size_t first = image_.disk_ok ? first_replaced : 0;
            const std::size_t start = static_cast<std::size_t>(image_.offsets[first == npos ? 0 : first]);
            std::string tail;
            tail.reserve(image_.content.size() - start + 256);
            std::size_t cursor = start;
            std::vector<std::pair<std::size_t, std::int64_t>> deltas;  // (line, size change)
            for (const Change& c : changes) {
                if (c.append) break;
                const std::size_t off = static_cast<std::size_t>(image_.offsets[c.idx]);
                const std::size_t old_len = static_cast<std::size_t>(image_.offsets[c.idx + 1]) - off - 1;
                tail.append(image_.content, cursor, off - cursor);
                tail += c.line;
                tail.push_back('\n');
                cursor = off + old_len + 1;
                if (c.line.size() != old_len)
                    deltas.emplace_back(c.idx, static_cast<std::int64_t>(c.line.size()) -
                                                   static_cast<std::int64_t>(old_len));
            }
            tail.append(image_.content, cursor, image_.content.size() - cursor);
            for (const Change& c : changes) {
                if (!c.append) continue;
                tail += c.line;
                tail.push_back('\n');
            }
            image_.content.resize(start);
            image_.content += tail;

            // shift the offsets of the lines behind each resized line
            std::int64_t shift = 0;
            std::size_t di = 0;
            for (std::size_t i = first == npos ? 0 : first; i < image_.offsets.size(); ++i) {
                while (di < deltas.size() && deltas[di].first < i) shift += deltas[di++].second;
                image_.offsets[i] = static_cast<std::uint64_t>(static_cast<std::int64_t>(image_.offsets[i]) + shift);
            }
            for (const Change& c : changes) {
                if (c.append) register_line(c.key, c.line.size(), int_pk);
            }
            image_.sig = start == 0 ? io::write_file(filename_, image_.content)
                                    : io::write_tail(filename_, start,
                                                     std::string_view(image_.content).substr(start),
                                                     image_.content.size());
            image_.disk_ok = true;
        } catch (...) {
            image_.valid = false;  // the file and our copy may disagree now: read it again next time
            throw;
        }
    }

    std::string filename_;
    FileImage image_;
    bool all_loaded_ = false;  // every line of the file has an object in memory
    bool has_issued_ = false;
    std::int64_t last_issued_ = 0;

    std::vector<std::unique_ptr<DBTableType>> owned_;
    std::vector<T*> objs_;  // every object in memory, in creation / load order
    std::unordered_map<std::string, T*> claimed_;  // saved or loaded objects by key
    std::unordered_map<std::string, T*> pending_;  // created, not yet saved objects by key
    std::vector<T*> dirty_;                        // pending objects whose key was assigned
    std::unique_ptr<Instance<T>> proto_;
    std::vector<std::string> prefixes_;
};

}  // namespace orm_detail

// ===========================================================================
// DBManager<T>
// ===========================================================================
template <class T>
class DBManager {
public:
    DBManager() = delete;

    // Creates a new object (owned by the manager; never delete it).
    static T* create() { return table().create(); }

    // Saves every object of T that is in memory.
    static void save() { table().save_all(); }

    // Loads every object of the file into memory (objects already in memory are kept).
    static void load() { table().load_all(); }

    // The object with the given key, or nullptr.
    static T* search(std::string_view key) {
        auto& tb = table();
        if (tb.pk_kind() != orm_detail::FieldKind::String) return nullptr;
        return tb.find(std::string(key));
    }
    static T* search(std::int64_t key) {
        auto& tb = table();
        if (tb.pk_kind() != orm_detail::FieldKind::Int) return nullptr;
        return tb.find(std::to_string(key));
    }

    // Every object whose `field` satisfies `condition`.
    static Result<T> search(std::string_view field, const orm_detail::Condition& condition) {
        return table().search_where(field, condition);
    }

    // One more than the largest `name` key saved in the file (1 if there is none); every
    // call returns a new value.
    static IntType get_next_key(std::string_view name) {
        if (orm_detail::key_suppress_depth() > 0) return IntType(std::string(name), 0);
        return IntType(std::string(name), table().next_key());
    }

private:
    template <class U> friend class ForeignKey;
    template <class U> friend class ForeignKeyVector;
    template <class U> friend class Result;

    static orm_detail::Table<T>& table() {
        static orm_detail::Table<T> instance;
        return instance;
    }

    static T* find_by_json_(const std::string& raw_key) {
        return table().find(orm_detail::canonical_from_json(raw_key));
    }

    static void sort_(std::vector<T*>& items, std::string_view field, Ordering ordering) {
        table().sort(items, field, ordering);
    }
};

#endif  // SIMPLE_ORM_HPP
