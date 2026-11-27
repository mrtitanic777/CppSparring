#include "pdf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace sparring::pdf {

namespace {

// Anything that stops a read: the message is what the person is told.
struct Fail {
    std::string message;
};

constexpr const char* kLocked = "This PDF is locked (it needs a password), so I can't read it.";
constexpr const char* kDamaged = "This PDF looks damaged, so I can't read it.";
constexpr const char* kNotPdf = "This file doesn't look like a PDF.";
constexpr const char* kScanned = "This PDF is only pictures of pages (a scan), so there are no words in it for me to read.";

// =====================================================================================================================
// Flate
// =====================================================================================================================

struct Huff {
    std::vector<std::uint16_t> table;   // (symbol << 4) | length, indexed by the next `bits` bits (first bit lowest); 0 = no such code
    int bits = 0;
};

bool build_huff(Huff& h, const std::uint8_t* lens, int n) {
    int count[16] = {};
    for (int i = 0; i < n; ++i) ++count[lens[i]];
    count[0] = 0;
    int maxlen = 0;
    for (int l = 1; l < 16; ++l)
        if (count[l]) maxlen = l;
    if (maxlen == 0) {
        h.bits = 1;
        h.table.assign(2, 0);
        return true;
    }
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left = (left << 1) - count[l];
        if (left < 0) return false;   // more codes than fit
    }
    unsigned next[16] = {};
    unsigned code = 0;
    for (int l = 1; l < 16; ++l) {
        code = (code + static_cast<unsigned>(count[l - 1])) << 1;
        next[l] = code;
    }
    h.bits = maxlen;
    h.table.assign(static_cast<std::size_t>(1) << maxlen, 0);
    for (int s = 0; s < n; ++s) {
        const int l = lens[s];
        if (!l) continue;
        const unsigned c = next[l]++;
        unsigned rev = 0;
        for (int b = 0; b < l; ++b)
            if (c & (1u << b)) rev |= 1u << (l - 1 - b);
        for (std::size_t k = rev; k < h.table.size(); k += static_cast<std::size_t>(1) << l)
            h.table[k] = static_cast<std::uint16_t>((s << 4) | l);
    }
    return true;
}

struct Inflater {
    const std::uint8_t* p;
    std::size_t n;
    std::size_t pos = 0;
    std::uint64_t buf = 0;
    int cnt = 0;
    std::string& out;
    std::size_t limit;

    Inflater(const std::uint8_t* data, std::size_t size, std::string& o, std::size_t lim) : p(data), n(size), out(o), limit(lim) {}

    void refill() {
        while (cnt <= 56 && pos < n) {
            buf |= static_cast<std::uint64_t>(p[pos++]) << cnt;
            cnt += 8;
        }
    }
    bool bits(int k, unsigned& v) {
        if (k == 0) {
            v = 0;
            return true;
        }
        if (cnt < k) refill();
        if (cnt < k) return false;
        v = static_cast<unsigned>(buf & ((static_cast<std::uint64_t>(1) << k) - 1));
        buf >>= k;
        cnt -= k;
        return true;
    }
    bool symbol(const Huff& h, unsigned& sym) {
        if (cnt < h.bits) refill();
        const std::uint16_t e = h.table[static_cast<std::size_t>(buf & ((static_cast<std::uint64_t>(1) << h.bits) - 1))];
        const int len = e & 15;
        if (len == 0 || len > cnt) return false;
        buf >>= len;
        cnt -= len;
        sym = e >> 4;
        return true;
    }

    bool run() {
        static const std::uint16_t len_base[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const std::uint8_t len_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const std::uint16_t dist_base[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const std::uint8_t dist_extra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        Huff fixed_lit, fixed_dist;
        bool have_fixed = false;
        for (;;) {
            unsigned final_block = 0, type = 0;
            if (!bits(1, final_block) || !bits(2, type)) return false;
            Huff dyn_lit, dyn_dist;
            const Huff* lit = nullptr;
            const Huff* dist = nullptr;
            if (type == 0) {
                buf >>= cnt & 7;   // to the next whole byte
                cnt -= cnt & 7;
                unsigned len = 0, nlen = 0;
                if (!bits(16, len) || !bits(16, nlen) || (len ^ 0xFFFF) != nlen) return false;
                // the bytes still in the bit buffer come first
                while (len > 0 && cnt >= 8) {
                    unsigned b = 0;
                    bits(8, b);
                    out.push_back(static_cast<char>(b));
                    --len;
                }
                if (len > n - pos) {
                    out.append(reinterpret_cast<const char*>(p + pos), n - pos);
                    pos = n;
                    return false;
                }
                out.append(reinterpret_cast<const char*>(p + pos), len);
                pos += len;
                if (out.size() > limit) return false;
            } else if (type == 1 || type == 2) {
                if (type == 1) {
                    if (!have_fixed) {
                        std::uint8_t l[288];
                        for (int i = 0; i < 144; ++i) l[i] = 8;
                        for (int i = 144; i < 256; ++i) l[i] = 9;
                        for (int i = 256; i < 280; ++i) l[i] = 7;
                        for (int i = 280; i < 288; ++i) l[i] = 8;
                        build_huff(fixed_lit, l, 288);
                        std::uint8_t d[30];
                        for (std::uint8_t& x : d) x = 5;
                        build_huff(fixed_dist, d, 30);
                        have_fixed = true;
                    }
                    lit = &fixed_lit;
                    dist = &fixed_dist;
                } else {
                    unsigned hlit = 0, hdist = 0, hclen = 0;
                    if (!bits(5, hlit) || !bits(5, hdist) || !bits(4, hclen)) return false;
                    hlit += 257;
                    hdist += 1;
                    hclen += 4;
                    if (hlit > 286 || hdist > 30) return false;
                    static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                    std::uint8_t cl[19] = {};
                    for (unsigned i = 0; i < hclen; ++i) {
                        unsigned v = 0;
                        if (!bits(3, v)) return false;
                        cl[order[i]] = static_cast<std::uint8_t>(v);
                    }
                    Huff clh;
                    if (!build_huff(clh, cl, 19)) return false;
                    std::uint8_t lens[320] = {};
                    unsigned i = 0;
                    while (i < hlit + hdist) {
                        unsigned sym = 0;
                        if (!symbol(clh, sym)) return false;
                        if (sym < 16) {
                            lens[i++] = static_cast<std::uint8_t>(sym);
                            continue;
                        }
                        unsigned rep = 0, extra = 0, value = 0;
                        if (sym == 16) {
                            if (i == 0) return false;
                            value = lens[i - 1];
                            if (!bits(2, extra)) return false;
                            rep = 3 + extra;
                        } else if (sym == 17) {
                            if (!bits(3, extra)) return false;
                            rep = 3 + extra;
                        } else {
                            if (!bits(7, extra)) return false;
                            rep = 11 + extra;
                        }
                        if (i + rep > hlit + hdist) return false;
                        while (rep--) lens[i++] = static_cast<std::uint8_t>(value);
                    }
                    if (!build_huff(dyn_lit, lens, static_cast<int>(hlit)) || !build_huff(dyn_dist, lens + hlit, static_cast<int>(hdist))) return false;
                    lit = &dyn_lit;
                    dist = &dyn_dist;
                }
                for (;;) {
                    unsigned sym = 0;
                    if (!symbol(*lit, sym)) return false;
                    if (sym < 256) {
                        out.push_back(static_cast<char>(sym));
                        continue;
                    }
                    if (sym == 256) break;
                    sym -= 257;
                    if (sym >= 29) return false;
                    unsigned extra = 0, length = len_base[sym];
                    if (!bits(len_extra[sym], extra)) return false;
                    length += extra;
                    unsigned dsym = 0;
                    if (!symbol(*dist, dsym) || dsym >= 30) return false;
                    unsigned distance = dist_base[dsym];
                    if (!bits(dist_extra[dsym], extra)) return false;
                    distance += extra;
                    if (distance > out.size()) return false;
                    std::size_t from = out.size() - distance;
                    out.reserve(out.size() + length);
                    for (unsigned k = 0; k < length; ++k) out.push_back(out[from + k]);
                    if (out.size() > limit) return false;
                }
            } else {
                return false;
            }
            if (final_block) return true;
        }
    }
};

// =====================================================================================================================
// Objects
// =====================================================================================================================

struct Obj {
    enum Type : std::uint8_t { Null, Bool, Int, Real, Str, Name, Arr, Dict, Ref, Op } t = Null;
    double num = 0;
    std::string s;               // Str, Name, Op
    std::vector<Obj> a;          // Arr
    std::vector<std::string> keys;   // Dict
    std::vector<Obj> vals;       // Dict
    int ref = 0;                 // Ref
    bool stream = false;         // a Dict that has a stream after it
    std::size_t stream_pos = 0, stream_len = 0;

    bool is_num() const { return t == Int || t == Real; }
    int as_int() const { return static_cast<int>(num); }
    const Obj* get(const char* key) const {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &vals[i];
        return nullptr;
    }
};

const Obj& null_obj() {
    static const Obj n;
    return n;
}

inline bool is_ws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
inline bool is_delim(int c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }
inline int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Reads objects out of a block of bytes: a file, an object stream or a page's content.
struct Lex {
    const std::uint8_t* d;
    std::size_t n;
    std::size_t pos;
    bool content;   // page content: no "1 0 R", and words are operators

    Lex(const std::uint8_t* data, std::size_t size, std::size_t at = 0, bool in_content = false) : d(data), n(size), pos(at), content(in_content) {}

    bool eof() const { return pos >= n; }
    void skip_ws() {
        while (pos < n) {
            if (is_ws(d[pos])) {
                ++pos;
            } else if (d[pos] == '%') {
                while (pos < n && d[pos] != '\n' && d[pos] != '\r') ++pos;
            } else {
                break;
            }
        }
    }
    bool word_is(const char* w) {
        const std::size_t len = std::strlen(w);
        return pos + len <= n && std::memcmp(d + pos, w, len) == 0 && (pos + len == n || is_ws(d[pos + len]) || is_delim(d[pos + len]));
    }

    Obj name() {
        Obj o;
        o.t = Obj::Name;
        ++pos;   // the slash
        while (pos < n && !is_ws(d[pos]) && !is_delim(d[pos])) {
            if (d[pos] == '#' && pos + 2 < n && hexval(d[pos + 1]) >= 0 && hexval(d[pos + 2]) >= 0) {
                o.s += static_cast<char>(hexval(d[pos + 1]) * 16 + hexval(d[pos + 2]));
                pos += 3;
            } else {
                o.s += static_cast<char>(d[pos++]);
            }
        }
        return o;
    }

    Obj literal_string() {
        Obj o;
        o.t = Obj::Str;
        ++pos;
        int depth = 1;
        while (pos < n) {
            const int c = d[pos++];
            if (c == '\\') {
                if (pos >= n) break;
                const int e = d[pos++];
                switch (e) {
                case 'n': o.s += '\n'; break;
                case 'r': o.s += '\r'; break;
                case 't': o.s += '\t'; break;
                case 'b': o.s += '\b'; break;
                case 'f': o.s += '\f'; break;
                case '\r':
                    if (pos < n && d[pos] == '\n') ++pos;
                    break;
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        for (int k = 0; k < 2 && pos < n && d[pos] >= '0' && d[pos] <= '7'; ++k) v = v * 8 + (d[pos++] - '0');
                        o.s += static_cast<char>(v & 0xFF);
                    } else {
                        o.s += static_cast<char>(e);
                    }
                }
            } else if (c == '(') {
                ++depth;
                o.s += '(';
            } else if (c == ')') {
                if (--depth == 0) break;
                o.s += ')';
            } else {
                o.s += static_cast<char>(c);
            }
        }
        return o;
    }

    Obj hex_string() {
        Obj o;
        o.t = Obj::Str;
        ++pos;
        int hi = -1;
        while (pos < n && d[pos] != '>') {
            const int v = hexval(d[pos++]);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else {
                o.s += static_cast<char>(hi * 16 + v);
                hi = -1;
            }
        }
        if (hi >= 0) o.s += static_cast<char>(hi * 16);
        if (pos < n) ++pos;
        return o;
    }

    Obj number() {
        Obj o;
        std::size_t p = pos;
        bool neg = false, real = false;
        while (p < n && (d[p] == '+' || d[p] == '-')) {
            if (d[p] == '-') neg = true;
            ++p;
        }
        double v = 0, scale = 1;
        bool any = false;
        for (; p < n; ++p) {
            if (d[p] >= '0' && d[p] <= '9') {
                any = true;
                if (real) {
                    scale /= 10;
                    v += (d[p] - '0') * scale;
                } else {
                    v = v * 10 + (d[p] - '0');
                }
            } else if (d[p] == '.' && !real) {
                real = true;
            } else if (d[p] == '-' && any) {
                continue;   // "1-2": a bad file; keep going
            } else {
                break;
            }
        }
        if (p == pos) ++p;   // never stand still
        pos = p;
        o.t = real ? Obj::Real : Obj::Int;
        o.num = neg ? -v : v;
        return o;
    }

    // One object. `depth` bounds how deep arrays and dictionaries nest.
    Obj object(int depth = 0) {
        Obj o;
        skip_ws();
        if (pos >= n) return o;
        if (depth > 80) throw Fail{kDamaged};
        const int c = d[pos];
        if (c == '/') return name();
        if (c == '(') return literal_string();
        if (c == '<') {
            if (pos + 1 < n && d[pos + 1] == '<') {
                pos += 2;
                o.t = Obj::Dict;
                for (;;) {
                    skip_ws();
                    if (pos >= n) break;
                    if (d[pos] == '>') {
                        pos += (pos + 1 < n && d[pos + 1] == '>') ? 2 : 1;
                        break;
                    }
                    Obj key = object(depth + 1);
                    if (key.t == Obj::Op && (key.s == "endobj" || key.s == "stream" || key.s == "endstream")) {
                        pos -= key.s.size();
                        break;
                    }
                    if (key.t != Obj::Name) continue;
                    Obj val = object(depth + 1);
                    if (val.t == Obj::Op && (val.s == "endobj" || val.s == "stream" || val.s == "endstream")) {
                        pos -= val.s.size();
                        break;
                    }
                    o.keys.push_back(std::move(key.s));
                    o.vals.push_back(std::move(val));
                }
                return o;
            }
            return hex_string();
        }
        if (c == '[') {
            ++pos;
            o.t = Obj::Arr;
            for (;;) {
                skip_ws();
                if (pos >= n) break;
                if (d[pos] == ']') {
                    ++pos;
                    break;
                }
                Obj item = object(depth + 1);
                if (item.t == Obj::Op && (item.s == "endobj" || item.s == "stream" || item.s == "endstream")) {
                    pos -= item.s.size();
                    break;
                }
                o.a.push_back(std::move(item));
                if (o.a.size() > 4000000) throw Fail{kDamaged};
            }
            return o;
        }
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            Obj num = number();
            if (!content && num.t == Obj::Int && num.num >= 0) {
                const std::size_t save = pos;
                skip_ws();
                if (pos < n && d[pos] >= '0' && d[pos] <= '9') {
                    Obj gen = number();
                    skip_ws();
                    if (gen.t == Obj::Int && pos < n && d[pos] == 'R' && (pos + 1 == n || is_ws(d[pos + 1]) || is_delim(d[pos + 1]))) {
                        ++pos;
                        Obj r;
                        r.t = Obj::Ref;
                        r.ref = num.as_int();
                        return r;
                    }
                }
                pos = save;
            }
            return num;
        }
        // a word
        const std::size_t start = pos;
        if (is_delim(c)) {
            ++pos;
        } else {
            while (pos < n && !is_ws(d[pos]) && !is_delim(d[pos])) ++pos;
        }
        const std::string w(reinterpret_cast<const char*>(d + start), pos - start);
        if (w == "true" || w == "false") {
            o.t = Obj::Bool;
            o.num = w == "true";
            return o;
        }
        if (w == "null" && !content) return o;
        o.t = Obj::Op;
        o.s = w;
        return o;
    }
};

// =====================================================================================================================
// Predictors and the other stream filters
// =====================================================================================================================

void undo_predictor(std::string& data, int predictor, int colors, int bpc, int columns) {
    if (predictor < 2 || columns <= 0 || colors <= 0 || bpc <= 0) return;
    const std::size_t bpp = std::max<std::size_t>(1, static_cast<std::size_t>(colors * bpc) / 8);
    const std::size_t row = (static_cast<std::size_t>(colors) * static_cast<std::size_t>(bpc) * static_cast<std::size_t>(columns) + 7) / 8;
    if (row == 0 || row > 64u * 1024 * 1024) return;
    if (predictor == 2) {
        if (bpc != 8) return;
        for (std::size_t r = 0; r + row <= data.size(); r += row)
            for (std::size_t i = bpp; i < row; ++i) data[r + i] = static_cast<char>(static_cast<std::uint8_t>(data[r + i]) + static_cast<std::uint8_t>(data[r + i - bpp]));
        return;
    }
    const std::size_t stride = row + 1;
    const std::size_t rows = (data.size() + stride - 1) / stride;
    std::string out;
    out.reserve(rows * row);
    std::string prev(row, '\0'), cur(row, '\0');
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t at = r * stride;
        if (at >= data.size()) break;
        const int type = static_cast<std::uint8_t>(data[at]);
        const std::size_t have = std::min(row, data.size() - at - 1);
        std::fill(cur.begin(), cur.end(), '\0');
        std::memcpy(cur.data(), data.data() + at + 1, have);
        for (std::size_t i = 0; i < row; ++i) {
            const int left = i >= bpp ? static_cast<std::uint8_t>(cur[i - bpp]) : 0;
            const int up = static_cast<std::uint8_t>(prev[i]);
            const int upleft = i >= bpp ? static_cast<std::uint8_t>(prev[i - bpp]) : 0;
            int v = static_cast<std::uint8_t>(cur[i]);
            switch (type) {
            case 1: v += left; break;
            case 2: v += up; break;
            case 3: v += (left + up) / 2; break;
            case 4: {
                const int p = left + up - upleft;
                const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - upleft);
                v += (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : upleft);
                break;
            }
            default: break;
            }
            cur[i] = static_cast<char>(v & 0xFF);
        }
        out.append(cur);
        prev = cur;
    }
    data.swap(out);
}

std::string ascii_hex(const std::string& in) {
    std::string out;
    int hi = -1;
    for (char c : in) {
        if (c == '>') break;
        const int v = hexval(static_cast<unsigned char>(c));
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else {
            out += static_cast<char>(hi * 16 + v);
            hi = -1;
        }
    }
    if (hi >= 0) out += static_cast<char>(hi * 16);
    return out;
}

std::string ascii85(const std::string& in) {
    std::string out;
    std::uint32_t group[5];
    int n = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (is_ws(c)) continue;
        if (c == '~') break;
        if (c == 'z' && n == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') continue;
        group[n++] = c - '!';
        if (n == 5) {
            std::uint32_t v = 0;
            for (int k = 0; k < 5; ++k) v = v * 85 + group[k];
            for (int k = 3; k >= 0; --k) out += static_cast<char>((v >> (8 * k)) & 0xFF);
            n = 0;
        }
    }
    if (n > 1) {
        for (int k = n; k < 5; ++k) group[k] = 84;
        std::uint32_t v = 0;
        for (int k = 0; k < 5; ++k) v = v * 85 + group[k];
        for (int k = 3; k > 3 - (n - 1); --k) out += static_cast<char>((v >> (8 * k)) & 0xFF);
    }
    return out;
}

std::string run_length(const std::string& in) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size()) {
        const int len = static_cast<unsigned char>(in[i++]);
        if (len == 128) break;
        if (len < 128) {
            const std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(len) + 1, in.size() - i);
            out.append(in, i, take);
            i += take;
        } else if (i < in.size()) {
            out.append(static_cast<std::size_t>(257 - len), in[i++]);
        }
    }
    return out;
}

std::string lzw(const std::string& in, int early) {
    std::string out;
    std::vector<std::pair<int, std::uint8_t>> table;   // (previous code, last byte) for codes 258 and up
    auto expand = [&](int code, std::string& s) {
        s.clear();
        while (code >= 258) {
            const std::size_t k = static_cast<std::size_t>(code - 258);
            if (k >= table.size()) return false;
            s += static_cast<char>(table[k].second);
            code = table[k].first;
        }
        s += static_cast<char>(code);
        std::reverse(s.begin(), s.end());
        return true;
    };
    int bits = 9;
    std::uint32_t buf = 0;
    int cnt = 0;
    int prev = -1;
    std::string prev_s, cur_s;
    for (std::size_t i = 0; i < in.size();) {
        while (cnt < bits && i < in.size()) {
            buf = (buf << 8) | static_cast<std::uint8_t>(in[i++]);
            cnt += 8;
        }
        if (cnt < bits) break;
        const int code = static_cast<int>((buf >> (cnt - bits)) & ((1u << bits) - 1));
        cnt -= bits;
        if (code == 256) {
            table.clear();
            bits = 9;
            prev = -1;
            continue;
        }
        if (code == 257) break;
        if (code < 256) {
            cur_s.assign(1, static_cast<char>(code));
        } else if (static_cast<std::size_t>(code - 258) < table.size()) {
            if (!expand(code, cur_s)) break;
        } else if (code - 258 == static_cast<int>(table.size()) && prev >= 0) {
            cur_s = prev_s + prev_s[0];
        } else {
            break;
        }
        out += cur_s;
        if (prev >= 0 && table.size() < 4000) table.emplace_back(prev, static_cast<std::uint8_t>(cur_s[0]));
        prev = code;
        prev_s = cur_s;
        const int size = static_cast<int>(table.size()) + 258 + early;
        bits = size >= 4096 ? 12 : size >= 2048 ? 12 : size >= 1024 ? 11 : size >= 512 ? 10 : 9;
        if (out.size() > 256u * 1024 * 1024) break;
    }
    return out;
}

// =====================================================================================================================
// Characters: names, encodings and ToUnicode maps
// =====================================================================================================================

void put_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// UTF-16BE bytes to UTF-8
std::string utf16be_to_utf8(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        unsigned u = (static_cast<std::uint8_t>(s[i]) << 8) | static_cast<std::uint8_t>(s[i + 1]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < s.size()) {
            const unsigned lo = (static_cast<std::uint8_t>(s[i + 2]) << 8) | static_cast<std::uint8_t>(s[i + 3]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        put_utf8(out, u);
    }
    return out;
}

const unsigned short kCp1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                    0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

struct GlyphName {
    const char* name;
    unsigned cp;
};
const GlyphName kGlyphNames[] = {
    {"space", 32}, {"exclam", 33}, {"quotedbl", 34}, {"numbersign", 35}, {"dollar", 36}, {"percent", 37}, {"ampersand", 38}, {"quotesingle", 39},
    {"parenleft", 40}, {"parenright", 41}, {"asterisk", 42}, {"plus", 43}, {"comma", 44}, {"hyphen", 45}, {"minus", 0x2212}, {"period", 46}, {"slash", 47},
    {"zero", 48}, {"one", 49}, {"two", 50}, {"three", 51}, {"four", 52}, {"five", 53}, {"six", 54}, {"seven", 55}, {"eight", 56}, {"nine", 57},
    {"colon", 58}, {"semicolon", 59}, {"less", 60}, {"equal", 61}, {"greater", 62}, {"question", 63}, {"at", 64}, {"bracketleft", 91},
    {"backslash", 92}, {"bracketright", 93}, {"asciicircum", 94}, {"underscore", 95}, {"grave", 96}, {"braceleft", 123}, {"bar", 124},
    {"braceright", 125}, {"asciitilde", 126}, {"bullet", 0x2022}, {"endash", 0x2013}, {"emdash", 0x2014}, {"quoteleft", 0x2018}, {"quoteright", 0x2019},
    {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotesinglbase", 0x201A}, {"quotedblbase", 0x201E}, {"ellipsis", 0x2026},
    {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04}, {"copyright", 0xA9}, {"registered", 0xAE}, {"trademark", 0x2122},
    {"degree", 0xB0}, {"plusminus", 0xB1}, {"multiply", 0xD7}, {"divide", 0xF7}, {"section", 0xA7}, {"paragraph", 0xB6}, {"dagger", 0x2020},
    {"daggerdbl", 0x2021}, {"guillemotleft", 0xAB}, {"guillemotright", 0xBB}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A},
    {"periodcentered", 0xB7}, {"nbspace", 0xA0}, {"nonbreakingspace", 0xA0}, {"sfthyphen", 0xAD}, {"exclamdown", 0xA1}, {"questiondown", 0xBF},
    {"cent", 0xA2}, {"sterling", 0xA3}, {"currency", 0xA4}, {"yen", 0xA5}, {"Euro", 0x20AC}, {"acute", 0xB4}, {"dieresis", 0xA8}, {"macron", 0xAF},
    {"cedilla", 0xB8}, {"circumflex", 0x2C6}, {"tilde", 0x2DC}, {"mu", 0xB5}, {"onehalf", 0xBD}, {"onequarter", 0xBC}, {"threequarters", 0xBE},
    {"eacute", 0xE9}, {"egrave", 0xE8}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"acircumflex", 0xE2}, {"adieresis", 0xE4}, {"aring", 0xE5},
    {"ccedilla", 0xE7}, {"ecircumflex", 0xEA}, {"edieresis", 0xEB}, {"iacute", 0xED}, {"igrave", 0xEC}, {"icircumflex", 0xEE}, {"idieresis", 0xEF},
    {"ntilde", 0xF1}, {"oacute", 0xF3}, {"ograve", 0xF2}, {"ocircumflex", 0xF4}, {"odieresis", 0xF6}, {"uacute", 0xFA}, {"ugrave", 0xF9},
    {"ucircumflex", 0xFB}, {"udieresis", 0xFC}, {"germandbls", 0xDF}, {"arrowright", 0x2192}, {"arrowleft", 0x2190}, {"lessequal", 0x2264},
    {"greaterequal", 0x2265}, {"notequal", 0x2260}, {"infinity", 0x221E}, {"fraction", 0x2044}, {"florin", 0x192}, {"perthousand", 0x2030},
    {"underscoredbl", 0x2017}, {"dotlessi", 0x131},
};

void glyph_to_utf8(const std::string& name, std::string& out) {
    if (name.size() == 1) {
        out += name;
        return;
    }
    static const std::unordered_map<std::string, unsigned> table = [] {
        std::unordered_map<std::string, unsigned> t;
        for (const GlyphName& g : kGlyphNames) t.emplace(g.name, g.cp);
        return t;
    }();
    const auto it = table.find(name);
    if (it != table.end()) {
        put_utf8(out, it->second);
        return;
    }
    if ((name.size() == 7 || name.size() == 5) && name.compare(0, 3, "uni") == 0 && name.size() == 7) {
        put_utf8(out, static_cast<unsigned>(std::strtoul(name.c_str() + 3, nullptr, 16)));
        return;
    }
    if (name.size() >= 5 && name[0] == 'u' && hexval(static_cast<unsigned char>(name[1])) >= 0 && name.size() <= 7) {
        put_utf8(out, static_cast<unsigned>(std::strtoul(name.c_str() + 1, nullptr, 16)));
        return;
    }
    // "f_i", "a.alt": the part before the dot or the underscore parts
    const std::size_t dot = name.find('.');
    if (dot != std::string::npos && dot > 0) {
        glyph_to_utf8(name.substr(0, dot), out);
        return;
    }
    const std::size_t us = name.find('_');
    if (us != std::string::npos && us > 0) {
        glyph_to_utf8(name.substr(0, us), out);
        glyph_to_utf8(name.substr(us + 1), out);
        return;
    }
}

struct CMap {
    std::unordered_map<std::uint32_t, std::string> chars;
    struct Range {
        std::uint32_t lo, hi;
        std::string base;
    };
    std::vector<Range> big;   // ranges too long to write out
    int min_len = 0, max_len = 0;

    bool lookup(std::uint32_t code, std::string& out) const {
        const auto it = chars.find(code);
        if (it != chars.end()) {
            out += it->second;
            return true;
        }
        for (const Range& r : big)
            if (code >= r.lo && code <= r.hi) {
                std::string v = r.base;
                add_to_last(v, code - r.lo);
                out += v;
                return true;
            }
        return false;
    }
    static void add_to_last(std::string& utf8, std::uint32_t add) {
        if (utf8.empty() || add == 0) return;
        // the last character of the UTF-8 text goes up by `add`
        std::size_t start = utf8.size() - 1;
        while (start > 0 && (static_cast<std::uint8_t>(utf8[start]) & 0xC0) == 0x80) --start;
        unsigned cp = 0;
        const std::uint8_t b = static_cast<std::uint8_t>(utf8[start]);
        const std::size_t len = utf8.size() - start;
        if (len == 1) cp = b;
        else if (len == 2) cp = ((b & 0x1F) << 6) | (utf8[start + 1] & 0x3F);
        else if (len == 3) cp = ((b & 0x0F) << 12) | ((utf8[start + 1] & 0x3F) << 6) | (utf8[start + 2] & 0x3F);
        else cp = ((b & 0x07) << 18) | ((utf8[start + 1] & 0x3F) << 12) | ((utf8[start + 2] & 0x3F) << 6) | (utf8[start + 3] & 0x3F);
        utf8.resize(start);
        put_utf8(utf8, cp + add);
    }
};

void parse_cmap(const std::string& text, CMap& cm) {
    Lex lx(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), 0, true);
    std::vector<Obj> stack;
    auto hex_code = [](const std::string& s) {
        std::uint32_t v = 0;
        for (unsigned char c : s) v = (v << 8) | c;
        return v;
    };
    std::size_t guard = 0;
    while (!lx.eof() && ++guard < 4000000) {
        Obj o = lx.object();
        if (o.t != Obj::Op) {
            stack.push_back(std::move(o));
            if (stack.size() > 100000) stack.erase(stack.begin(), stack.begin() + 50000);
            continue;
        }
        const std::string& op = o.s;
        if (op == "endcodespacerange") {
            for (const Obj& s : stack)
                if (s.t == Obj::Str && !s.s.empty()) {
                    const int len = static_cast<int>(s.s.size());
                    cm.min_len = cm.min_len ? std::min(cm.min_len, len) : len;
                    cm.max_len = std::max(cm.max_len, len);
                }
            stack.clear();
        } else if (op == "endbfchar") {
            for (std::size_t i = 0; i + 1 < stack.size(); i += 2)
                if (stack[i].t == Obj::Str && stack[i + 1].t == Obj::Str) cm.chars[hex_code(stack[i].s)] = utf16be_to_utf8(stack[i + 1].s);
            stack.clear();
        } else if (op == "endbfrange") {
            for (std::size_t i = 0; i + 2 < stack.size(); i += 3) {
                if (stack[i].t != Obj::Str || stack[i + 1].t != Obj::Str) continue;
                const std::uint32_t lo = hex_code(stack[i].s), hi = hex_code(stack[i + 1].s);
                if (hi < lo) continue;
                const Obj& dst = stack[i + 2];
                if (dst.t == Obj::Arr) {
                    for (std::size_t k = 0; k < dst.a.size() && lo + k <= hi; ++k)
                        if (dst.a[k].t == Obj::Str) cm.chars[lo + static_cast<std::uint32_t>(k)] = utf16be_to_utf8(dst.a[k].s);
                } else if (dst.t == Obj::Str) {
                    const std::string base = utf16be_to_utf8(dst.s);
                    if (hi - lo <= 512) {
                        for (std::uint32_t c = lo; c <= hi; ++c) {
                            std::string v = base;
                            CMap::add_to_last(v, c - lo);
                            cm.chars[c] = std::move(v);
                        }
                    } else {
                        cm.big.push_back({lo, hi, base});
                    }
                }
            }
            stack.clear();
        } else if (op.compare(0, 5, "begin") == 0 || op.compare(0, 3, "end") == 0) {
            stack.clear();
        }
    }
}

// =====================================================================================================================
// Fonts
// =====================================================================================================================

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

float approx_width(unsigned char c) {   // a rough width for fonts that give none
    if (c == ' ') return 250;
    if (std::strchr("iljtf.,;:'|!I", c)) return 278;
    if (std::strchr("mwMW", c)) return 800;
    if (c >= 'A' && c <= 'Z') return 667;
    return 500;
}

struct Font {
    bool two_byte = false;
    bool mono = false, bold = false;
    bool mapped = true;   // there is a way to turn codes into letters
    CMap to_unicode;
    bool has_to_unicode = false;
    std::vector<std::string> simple;   // simple fonts: the text for each of the 256 codes
    std::vector<float> widths;         // simple fonts: per code, 1000 = one em (negative: not given)
    float missing_w = -1;
    bool std14_mono = false;
    std::unordered_map<std::uint32_t, float> cid_w;
    float dw = 1000;
    std::unordered_map<std::uint32_t, std::string> memo;

    float width(std::uint32_t code) const {
        if (two_byte) {
            const auto it = cid_w.find(code);
            return it != cid_w.end() ? it->second : dw;
        }
        if (code < widths.size() && widths[code] >= 0) return widths[code];
        if (missing_w >= 0) return missing_w;
        if (mono) return 600;
        return approx_width(static_cast<unsigned char>(code));
    }
    const std::string& text(std::uint32_t code) {
        const auto it = memo.find(code);
        if (it != memo.end()) return it->second;
        std::string out;
        if (has_to_unicode && to_unicode.lookup(code, out)) {
        } else if (!two_byte && code < simple.size()) {
            out = simple[code];
        }
        return memo.emplace(code, std::move(out)).first->second;
    }
};

// =====================================================================================================================
// The file
// =====================================================================================================================

struct XEntry {
    int type = 0;   // 0 free, 1 at an offset, 2 inside an object stream
    std::uint64_t a = 0;   // offset, or the object stream's number
    int b = 0;             // the position inside the object stream
};

struct ObjStm {
    std::string data;
    std::vector<std::pair<int, std::size_t>> items;   // object number, offset from `first`
    std::size_t first = 0;
};

struct PageInfo {
    int num = 0;
    const Obj* resources = nullptr;
    const Obj* mediabox = nullptr;
};

class Reader {
public:
    Reader(const std::string& bytes, const ProgressFn& progress) : buf_(bytes), progress_(progress) {
        data_ = reinterpret_cast<const std::uint8_t*>(buf_.data());
        size_ = buf_.size();
    }

    Document run() {
        Document doc;
        try {
            open();
            started_ = true;
            std::vector<PageInfo> pages = collect_pages();
            doc.title = title();
            const int total = static_cast<int>(pages.size());
            int done = 0;
            std::size_t chars = 0;
            for (const PageInfo& p : pages) {
                if (progress_ && !progress_(done, total)) {
                    doc.cancelled = true;
                    doc.error = "Stopped.";
                    return doc;
                }
                Page page;
                try {
                    page = read_page(p);
                } catch (const Fail& f) {
                    if (f.message == kLocked) throw;
                    // one bad page does not stop the book
                }
                for (const Line& l : page.lines) chars += l.text.size();
                doc.pages.push_back(std::move(page));
                ++done;
            }
            if (progress_) progress_(done, total);
            if (doc.pages.empty()) throw Fail{kDamaged};
            if (chars < 20) throw Fail{kScanned};
            doc.ok = true;
        } catch (const Fail& f) {
            doc.ok = false;
            doc.error = f.message;
            doc.pages.clear();
        } catch (const std::bad_alloc&) {
            doc.ok = false;
            doc.error = "This PDF is too big for me to read.";
            doc.pages.clear();
        } catch (...) {
            doc.ok = false;
            doc.error = kDamaged;
            doc.pages.clear();
        }
        return doc;
    }

private:
    const std::string& buf_;
    const std::uint8_t* data_;
    std::size_t size_;
    const ProgressFn& progress_;
    std::size_t base_ = 0;   // junk before %PDF shifts every offset by this

    std::unordered_map<int, XEntry> xref_;
    Obj trailer_;
    bool have_trailer_ = false;
    std::unordered_map<int, Obj> cache_;
    std::unordered_map<int, std::unique_ptr<ObjStm>> objstms_;
    std::size_t objstm_bytes_ = 0;
    bool rebuilt_ = false;
    bool started_ = false;
    int depth_ = 0;
    int get_depth_ = 0;
    std::map<const void*, std::unique_ptr<Font>> fonts_;
    std::size_t forms_this_page_ = 0;

    // ---- opening
    void open() {
        std::size_t head = std::string::npos;
        const std::size_t scan = std::min<std::size_t>(size_, 4096);
        for (std::size_t i = 0; i + 5 <= scan; ++i)
            if (std::memcmp(data_ + i, "%PDF-", 5) == 0) {
                head = i;
                break;
            }
        if (head == std::string::npos) throw Fail{kNotPdf};
        base_ = head;
        bool ok = false;
        try {
            ok = read_xref_chain();
        } catch (const Fail& f) {
            if (f.message == kLocked) throw;
        }
        if (!ok || !root_ok()) rebuild();
        if (!have_trailer_ || !trailer_.get("Root")) throw Fail{kDamaged};
        if (trailer_.get("Encrypt")) throw Fail{kLocked};
    }

    bool root_ok() {
        const Obj* r = trailer_.get("Root");
        if (!r) return false;
        const Obj& root = resolve(*r);
        return root.t == Obj::Dict && root.get("Pages") != nullptr;
    }

    bool read_xref_chain() {
        std::size_t at = std::string::npos;
        if (size_ < 9) return false;
        const std::size_t from = size_ > 4096 ? size_ - 4096 : 0;
        for (std::size_t i = size_ - 9;; --i) {
            if (std::memcmp(data_ + i, "startxref", 9) == 0) {
                Lex lx(data_, size_, i + 9);
                Obj o = lx.object();
                if (o.t == Obj::Int) at = static_cast<std::size_t>(o.num);
                break;
            }
            if (i <= from) break;
        }
        if (at == std::string::npos) return false;
        std::set<std::size_t> seen;
        std::vector<std::size_t> todo{at};
        bool first = true;
        while (!todo.empty()) {
            const std::size_t off = todo.back() + base_;
            todo.pop_back();
            if (off >= size_ || !seen.insert(off).second) continue;
            Lex lx(data_, size_, off);
            lx.skip_ws();
            Obj tr;
            if (lx.word_is("xref")) {
                lx.pos += 4;
                read_table(lx);
                lx.skip_ws();
                if (!lx.word_is("trailer")) return false;
                lx.pos += 7;
                tr = lx.object();
                if (tr.t != Obj::Dict) return false;
                if (const Obj* x = tr.get("XRefStm"); x && x->is_num()) todo.push_back(static_cast<std::size_t>(x->num));
            } else {
                Obj st;
                int num = 0;
                if (!parse_indirect(off, num, st) || !st.stream) return false;
                read_xref_stream(st);
                tr = st;
            }
            if (first) {
                trailer_ = tr;
                have_trailer_ = true;
                first = false;
            } else if (!trailer_.get("Root") && tr.get("Root")) {
                trailer_ = tr;
            }
            if (const Obj* prev = tr.get("Prev"); prev && prev->is_num()) todo.push_back(static_cast<std::size_t>(prev->num));
            // an older table is read before the newer one's hybrid part: keep the order simple (newest entries were added first)
        }
        return have_trailer_;
    }

    void read_table(Lex& lx) {
        for (;;) {
            lx.skip_ws();
            if (lx.eof() || lx.word_is("trailer")) return;
            Obj start = lx.object(), count = lx.object();
            if (start.t != Obj::Int || count.t != Obj::Int || count.num < 0 || count.num > 20000000) throw Fail{kDamaged};
            long first_num = static_cast<long>(start.num);
            for (long i = 0; i < static_cast<long>(count.num); ++i) {
                Obj off = lx.object(), gen = lx.object(), kind = lx.object();
                if (off.t != Obj::Int || gen.t != Obj::Int || kind.t != Obj::Op) throw Fail{kDamaged};
                if (i == 0 && first_num == 1 && kind.s == "f" && gen.num == 65535) first_num = 0;   // a table that counts from 1 by mistake
                const int num = static_cast<int>(first_num + i);
                XEntry e;
                if (kind.s == "n") {
                    e.type = 1;
                    e.a = static_cast<std::uint64_t>(off.num);
                }
                xref_.emplace(num, e);
            }
        }
    }

    void read_xref_stream(const Obj& st) {
        std::string data;
        if (!decode(st, data)) throw Fail{kDamaged};
        const Obj* w = st.get("W");
        if (!w || w->t != Obj::Arr || w->a.size() < 3) throw Fail{kDamaged};
        const int w0 = w->a[0].as_int(), w1 = w->a[1].as_int(), w2 = w->a[2].as_int();
        if (w0 < 0 || w1 < 0 || w2 < 0 || w0 > 8 || w1 > 8 || w2 > 8) throw Fail{kDamaged};
        const std::size_t row = static_cast<std::size_t>(w0 + w1 + w2);
        if (row == 0) throw Fail{kDamaged};
        std::vector<std::pair<long, long>> index;
        if (const Obj* ix = st.get("Index"); ix && ix->t == Obj::Arr) {
            for (std::size_t i = 0; i + 1 < ix->a.size(); i += 2) index.emplace_back(static_cast<long>(ix->a[i].num), static_cast<long>(ix->a[i + 1].num));
        } else {
            const Obj* sz = st.get("Size");
            index.emplace_back(0, sz ? static_cast<long>(sz->num) : static_cast<long>(data.size() / row));
        }
        std::size_t at = 0;
        auto field = [&](int width) {
            std::uint64_t v = 0;
            for (int i = 0; i < width; ++i) v = (v << 8) | static_cast<std::uint8_t>(data[at++]);
            return v;
        };
        for (const auto& [start, count] : index)
            for (long i = 0; i < count; ++i) {
                if (at + row > data.size()) return;
                const std::uint64_t t = w0 == 0 ? 1 : field(w0);
                const std::uint64_t a = field(w1);
                const std::uint64_t b = field(w2);
                XEntry e;
                if (t == 1) {
                    e.type = 1;
                    e.a = a;
                } else if (t == 2) {
                    e.type = 2;
                    e.a = a;
                    e.b = static_cast<int>(b);
                }
                xref_.emplace(static_cast<int>(start + i), e);
            }
    }

    // When the table is missing or wrong: find the objects by looking through the whole file.
    void rebuild() {
        if (rebuilt_) return;
        rebuilt_ = true;
        xref_.clear();
        if (!started_) {   // (once pages are being read, objects already in hand stay: things point into them)
            cache_.clear();
            objstms_.clear();
            objstm_bytes_ = 0;
        }
        have_trailer_ = false;
        trailer_ = Obj();
        const char* hay = buf_.data();
        std::vector<int> stream_objs, catalog;
        std::size_t at = 0;
        const std::string_view all(hay, size_);
        while ((at = all.find(" obj", at)) != std::string_view::npos) {
            std::size_t p = at;
            // back over the generation and the number
            std::size_t g_end = p;
            while (p > 0 && hay[p - 1] >= '0' && hay[p - 1] <= '9') --p;
            const std::size_t g_start = p;
            if (g_start == g_end || p == 0 || hay[p - 1] != ' ') {
                ++at;
                continue;
            }
            --p;
            std::size_t n_end = p;
            while (p > 0 && hay[p - 1] >= '0' && hay[p - 1] <= '9') --p;
            if (p == n_end || n_end - p > 9) {
                ++at;
                continue;
            }
            const int num = std::atoi(std::string(hay + p, n_end - p).c_str());
            XEntry e;
            e.type = 1;
            e.a = p - base_;
            xref_[num] = e;
            const std::size_t window = std::min<std::size_t>(size_ - at, 400);
            const std::string_view w(hay + at, window);
            if (w.find("/ObjStm") != std::string_view::npos) stream_objs.push_back(num);
            if (w.find("/XRef") != std::string_view::npos) {
                Obj st;
                int n2 = 0;
                if (parse_indirect(p, n2, st) && st.get("Root")) {
                    trailer_ = st;
                    have_trailer_ = true;
                }
            }
            if (w.find("/Catalog") != std::string_view::npos) catalog.push_back(num);
            at += 4;
        }
        // trailers
        std::size_t t = 0;
        while ((t = all.find("trailer", t)) != std::string_view::npos) {
            Lex lx(data_, size_, t + 7);
            Obj tr = lx.object();
            if (tr.t == Obj::Dict && tr.get("Root")) {
                trailer_ = tr;
                have_trailer_ = true;
            }
            t += 7;
        }
        // objects inside object streams
        for (int sn : stream_objs) {
            const ObjStm* os = load_objstm(sn);
            if (!os) continue;
            for (std::size_t i = 0; i < os->items.size(); ++i) {
                XEntry e;
                e.type = 2;
                e.a = static_cast<std::uint64_t>(sn);
                e.b = static_cast<int>(i);
                xref_.emplace(os->items[i].first, e);
            }
        }
        if (!have_trailer_ || !trailer_.get("Root")) {
            for (int c : catalog) {
                Obj tr;
                tr.t = Obj::Dict;
                tr.keys.push_back("Root");
                Obj r;
                r.t = Obj::Ref;
                r.ref = c;
                tr.vals.push_back(r);
                trailer_ = tr;
                have_trailer_ = true;
                if (root_ok()) break;
            }
        }
    }

    // ---- reading one object
    bool parse_indirect(std::size_t off, int& num, Obj& out) {
        if (off >= size_) return false;
        Lex lx(data_, size_, off);
        Obj n = lx.object(), g = lx.object(), kw = lx.object();
        if (n.t != Obj::Int || g.t != Obj::Int || kw.t != Obj::Op || kw.s != "obj") return false;
        num = n.as_int();
        out = lx.object();
        if (out.t == Obj::Op) {   // "endobj" straight away: an empty object
            out = Obj();
            return true;
        }
        if (out.t == Obj::Dict) {
            lx.skip_ws();
            if (lx.word_is("stream")) {
                lx.pos += 6;
                if (lx.pos < size_ && data_[lx.pos] == '\r' && lx.pos + 1 < size_ && data_[lx.pos + 1] == '\n') lx.pos += 2;
                else if (lx.pos < size_ && (data_[lx.pos] == '\n' || data_[lx.pos] == '\r')) lx.pos += 1;
                out.stream = true;
                out.stream_pos = lx.pos;
                long long len = -1;
                if (const Obj* l = out.get("Length")) {
                    if (l->t == Obj::Ref) {
                        if (depth_ < 8) {
                            ++depth_;
                            const Obj& r = get(l->ref);
                            --depth_;
                            if (r.is_num()) len = static_cast<long long>(r.num);
                        }
                    } else if (l->is_num()) {
                        len = static_cast<long long>(l->num);
                    }
                }
                bool good = false;
                if (len >= 0 && out.stream_pos + static_cast<std::size_t>(len) <= size_) {
                    Lex e(data_, size_, out.stream_pos + static_cast<std::size_t>(len));
                    e.skip_ws();
                    good = e.word_is("endstream") || e.word_is("endobj");
                }
                if (!good) {
                    const std::string_view all(buf_.data(), size_);
                    const std::size_t end = all.find("endstream", out.stream_pos);
                    std::size_t stop = end == std::string_view::npos ? size_ : end;
                    if (stop > out.stream_pos && data_[stop - 1] == '\n') --stop;
                    if (stop > out.stream_pos && data_[stop - 1] == '\r') --stop;
                    len = static_cast<long long>(stop - out.stream_pos);
                }
                out.stream_len = static_cast<std::size_t>(len);
            }
        }
        return true;
    }

    const ObjStm* load_objstm(int num) {
        const auto it = objstms_.find(num);
        if (it != objstms_.end()) return it->second.get();
        if (objstm_bytes_ > 192u * 1024 * 1024) {
            objstms_.clear();
            objstm_bytes_ = 0;
        }
        const auto xe = xref_.find(num);
        if (xe == xref_.end() || xe->second.type != 1) return nullptr;
        Obj st;
        int n = 0;
        if (!parse_indirect(static_cast<std::size_t>(xe->second.a) + base_, n, st) || !st.stream) return nullptr;
        auto os = std::make_unique<ObjStm>();
        if (!decode(st, os->data)) return nullptr;
        const Obj* cnt = st.get("N");
        const Obj* first = st.get("First");
        if (!cnt || !first) return nullptr;
        os->first = static_cast<std::size_t>(first->num);
        Lex lx(reinterpret_cast<const std::uint8_t*>(os->data.data()), os->data.size());
        const long count = static_cast<long>(cnt->num);
        for (long i = 0; i < count && i < 1000000; ++i) {
            Obj a = lx.object(), b = lx.object();
            if (a.t != Obj::Int || b.t != Obj::Int) break;
            os->items.emplace_back(a.as_int(), static_cast<std::size_t>(b.num));
        }
        objstm_bytes_ += os->data.size();
        const ObjStm* raw = os.get();
        objstms_.emplace(num, std::move(os));
        return raw;
    }

    const Obj& get(int num) {
        const auto c = cache_.find(num);
        if (c != cache_.end()) return c->second;
        const auto xe = xref_.find(num);
        if (xe == xref_.end() || xe->second.type == 0) return null_obj();
        if (get_depth_ > 48) return null_obj();   // objects that need each other to be read: a damaged or hostile file
        struct Depth {
            int& d;
            explicit Depth(int& x) : d(x) { ++d; }
            ~Depth() { --d; }
        } depth_guard(get_depth_);
        Obj obj;
        bool ok = false;
        if (xe->second.type == 1) {
            int found = 0;
            ok = parse_indirect(static_cast<std::size_t>(xe->second.a) + base_, found, obj) && found == num;
            if (!ok && !rebuilt_) {   // the table lied: look through the file
                rebuild();
                return get(num);
            }
        } else {
            const ObjStm* os = load_objstm(static_cast<int>(xe->second.a));
            if (os) {
                std::size_t idx = static_cast<std::size_t>(xe->second.b);
                if (idx >= os->items.size() || os->items[idx].first != num) {
                    idx = os->items.size();
                    for (std::size_t i = 0; i < os->items.size(); ++i)
                        if (os->items[i].first == num) idx = i;
                }
                if (idx < os->items.size()) {
                    Lex lx(reinterpret_cast<const std::uint8_t*>(os->data.data()), os->data.size(), os->first + os->items[idx].second);
                    obj = lx.object();
                    ok = true;
                }
            }
        }
        if (!ok) return null_obj();
        return cache_.emplace(num, std::move(obj)).first->second;
    }

    const Obj& resolve(const Obj& o) {
        const Obj* cur = &o;
        for (int i = 0; i < 12 && cur->t == Obj::Ref; ++i) cur = &get(cur->ref);
        return cur->t == Obj::Ref ? null_obj() : *cur;
    }
    const Obj* find(const Obj& dict, const char* key) {
        const Obj* v = dict.get(key);
        if (!v) return nullptr;
        const Obj& r = resolve(*v);
        return r.t == Obj::Null ? nullptr : &r;
    }

    // ---- stream data
    bool decode(const Obj& st, std::string& out, std::size_t limit = 128u * 1024 * 1024) {
        if (!st.stream || st.stream_pos > size_) return false;
        std::string data(buf_, st.stream_pos, std::min(st.stream_len, size_ - st.stream_pos));
        std::vector<std::string> filters;
        std::vector<const Obj*> parms;
        const Obj* f = find(st, "Filter");
        if (!f) f = find(st, "F");
        if (f && f->t == Obj::Name) {
            filters.push_back(f->s);
        } else if (f && f->t == Obj::Arr) {
            for (const Obj& x : f->a) {
                const Obj& r = resolve(x);
                if (r.t == Obj::Name) filters.push_back(r.s);
            }
        }
        const Obj* dp = find(st, "DecodeParms");
        if (!dp) dp = find(st, "DP");
        if (dp && dp->t == Obj::Dict) {
            parms.push_back(dp);
        } else if (dp && dp->t == Obj::Arr) {
            for (const Obj& x : dp->a) parms.push_back(&resolve(x));
        }
        for (std::size_t i = 0; i < filters.size(); ++i) {
            const std::string& name = filters[i];
            const Obj* pr = i < parms.size() && parms[i]->t == Obj::Dict ? parms[i] : nullptr;
            if (name == "FlateDecode" || name == "Fl") {
                std::string out2;
                inflate(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out2, true, limit);
                if (out2.empty() && !data.empty()) {
                    // not a zlib stream after all: try it as bare deflate
                    inflate(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out2, false, limit);
                }
                data.swap(out2);
            } else if (name == "ASCIIHexDecode" || name == "AHx") {
                data = ascii_hex(data);
            } else if (name == "ASCII85Decode" || name == "A85") {
                data = ascii85(data);
            } else if (name == "RunLengthDecode" || name == "RL") {
                data = run_length(data);
            } else if (name == "LZWDecode" || name == "LZW") {
                int early = 1;
                if (pr)
                    if (const Obj* e = find(*pr, "EarlyChange")) early = e->as_int();
                data = lzw(data, early);
            } else if (name == "Crypt") {
                continue;
            } else {
                return false;   // a picture format
            }
            if (pr && (name == "FlateDecode" || name == "Fl" || name == "LZWDecode" || name == "LZW")) {
                auto num = [&](const char* k, int dflt) {
                    const Obj* v = find(*pr, k);
                    return v && v->is_num() ? v->as_int() : dflt;
                };
                undo_predictor(data, num("Predictor", 1), num("Colors", 1), num("BitsPerComponent", 8), num("Columns", 1));
            }
        }
        out.swap(data);
        return true;
    }

    // ---- the page tree
    std::string title() {
        const Obj* info = trailer_.get("Info");
        if (!info) return {};
        const Obj& dict = resolve(*info);
        if (dict.t != Obj::Dict) return {};
        const Obj* t = find(dict, "Title");
        if (!t || t->t != Obj::Str) return {};
        std::string s = t->s;
        if (s.size() >= 2 && static_cast<std::uint8_t>(s[0]) == 0xFE && static_cast<std::uint8_t>(s[1]) == 0xFF) return utf16be_to_utf8(s.substr(2));
        if (s.size() >= 3 && static_cast<std::uint8_t>(s[0]) == 0xEF && static_cast<std::uint8_t>(s[1]) == 0xBB) return s.substr(3);
        std::string out;
        for (unsigned char c : s) put_utf8(out, c >= 0xA0 || c < 0x80 ? c : (c >= 0x80 && c < 0xA0 ? kCp1252[c - 0x80] : c));
        return out;
    }

    std::vector<PageInfo> collect_pages() {
        std::vector<PageInfo> pages;
        const Obj& root = resolve(*trailer_.get("Root"));
        const Obj* top = root.get("Pages");
        std::unordered_set<int> seen;
        struct Item {
            const Obj* node;
            int num;
            const Obj* res;
            const Obj* box;
        };
        std::vector<Item> stack;
        if (top) stack.push_back({top, top->t == Obj::Ref ? top->ref : 0, nullptr, nullptr});
        while (!stack.empty() && pages.size() < 6000) {
            Item it = stack.back();
            stack.pop_back();
            if (it.num && !seen.insert(it.num).second) continue;
            const Obj& node = resolve(*it.node);
            if (node.t != Obj::Dict) continue;
            const Obj* res = find(node, "Resources");
            const Obj* box = find(node, "MediaBox");
            if (!res) res = it.res;
            if (!box) box = it.box;
            const Obj* kids = find(node, "Kids");
            if (kids && kids->t == Obj::Arr) {
                for (std::size_t i = kids->a.size(); i-- > 0;) stack.push_back({&kids->a[i], kids->a[i].t == Obj::Ref ? kids->a[i].ref : 0, res, box});
            } else {
                if (it.num) pages.push_back({it.num, res, box});
            }
        }
        if (pages.empty()) {   // a broken tree: take every object that says it is a page
            std::vector<int> nums;
            for (const auto& [n, e] : xref_) nums.push_back(n);
            std::sort(nums.begin(), nums.end());
            for (int n : nums) {
                const Obj& o = get(n);
                if (o.t != Obj::Dict) continue;
                const Obj* ty = find(o, "Type");
                if (ty && ty->t == Obj::Name && ty->s == "Page") {
                    pages.push_back({n, find(o, "Resources"), find(o, "MediaBox")});
                }
                if (pages.size() >= 6000) break;
            }
        }
        return pages;
    }

    // ---- fonts
    Font* font_for(const Obj& dict_in) {
        const Obj& dict = resolve(dict_in);
        if (dict.t != Obj::Dict) return nullptr;
        auto& slot = fonts_[&dict];
        if (slot) return slot.get();
        slot = std::make_unique<Font>();
        load_font(dict, *slot);
        return slot.get();
    }

    static bool name_is_mono(const std::string& base) {
        const std::string n = lower(base);
        for (const char* w : {"courier", "mono", "consol", "code", "menlo", "inconsolata", "sourcecodepro", "typewriter", "cmtt", "lettergothic", "cousine", "ocr"})
            if (contains(n, w)) return true;
        return false;
    }
    static bool name_is_bold(const std::string& base) {
        const std::string n = lower(base);
        for (const char* w : {"bold", "black", "heavy", "semibold", "demi", "-bd", "extrabd"})
            if (contains(n, w)) return true;
        return false;
    }

    void load_font(const Obj& dict, Font& f) {
        std::string base;
        if (const Obj* b = find(dict, "BaseFont"); b && b->t == Obj::Name) base = b->s;
        if (const std::size_t plus = base.find('+'); plus == 6) base = base.substr(7);
        const Obj* subtype = find(dict, "Subtype");
        const std::string sub = subtype && subtype->t == Obj::Name ? subtype->s : "";
        const Obj* desc = find(dict, "FontDescriptor");
        const Obj* cid = nullptr;
        if (sub == "Type0") {
            f.two_byte = true;
            if (const Obj* d = find(dict, "DescendantFonts"); d && d->t == Obj::Arr && !d->a.empty()) {
                const Obj& c = resolve(d->a[0]);
                if (c.t == Obj::Dict) {
                    cid = &c;
                    desc = find(c, "FontDescriptor");
                    if (base.empty())
                        if (const Obj* b = find(c, "BaseFont"); b && b->t == Obj::Name) base = b->s;
                }
            }
        }
        f.mono = name_is_mono(base);
        f.bold = name_is_bold(base);
        if (desc && desc->t == Obj::Dict) {
            if (const Obj* fl = find(*desc, "Flags"); fl && fl->is_num()) {
                const int flags = fl->as_int();
                if (flags & 1) f.mono = true;
                if (flags & 0x40000) f.bold = true;
            }
            if (const Obj* fw = find(*desc, "FontWeight"); fw && fw->is_num() && fw->num >= 600) f.bold = true;
            if (const Obj* mw = find(*desc, "MissingWidth"); mw && mw->is_num()) f.missing_w = static_cast<float>(mw->num);
            if (!find(*desc, "FontName") && base.empty()) {
            } else if (const Obj* fnm = find(*desc, "FontName"); fnm && fnm->t == Obj::Name) {
                std::string n = fnm->s;
                if (n.size() > 7 && n[6] == '+') n = n.substr(7);
                if (name_is_mono(n)) f.mono = true;
                if (name_is_bold(n)) f.bold = true;
            }
        }
        // letters
        if (const Obj* tu = find(dict, "ToUnicode"); tu && tu->stream) {
            std::string text;
            if (decode(*tu, text, 16u * 1024 * 1024)) {
                parse_cmap(text, f.to_unicode);
                f.has_to_unicode = !f.to_unicode.chars.empty() || !f.to_unicode.big.empty();
            }
        }
        if (f.two_byte) {
            if (const Obj* enc = find(dict, "Encoding"); enc && enc->stream) {
                // an embedded code map: if it says one-byte codes, believe it
            }
            f.dw = 1000;
            if (cid) {
                if (const Obj* dw = find(*cid, "DW"); dw && dw->is_num()) f.dw = static_cast<float>(dw->num);
                if (const Obj* w = find(*cid, "W"); w && w->t == Obj::Arr) {
                    for (std::size_t i = 0; i < w->a.size();) {
                        const Obj& first = resolve(w->a[i]);
                        if (!first.is_num() || i + 1 >= w->a.size()) break;
                        const Obj& second = resolve(w->a[i + 1]);
                        if (second.t == Obj::Arr) {
                            std::uint32_t c = static_cast<std::uint32_t>(first.num);
                            for (const Obj& x : second.a) f.cid_w[c++] = static_cast<float>(resolve(x).num);
                            i += 2;
                        } else if (second.is_num() && i + 2 < w->a.size()) {
                            const std::uint32_t lo = static_cast<std::uint32_t>(first.num), hi = static_cast<std::uint32_t>(second.num);
                            const float wd = static_cast<float>(resolve(w->a[i + 2]).num);
                            if (hi >= lo && hi - lo < 70000)
                                for (std::uint32_t c = lo; c <= hi; ++c) f.cid_w[c] = wd;
                            i += 3;
                        } else {
                            break;
                        }
                    }
                }
            }
            f.mapped = f.has_to_unicode;
            return;
        }
        // a font of one-byte codes
        f.simple.assign(256, std::string());
        f.widths.assign(256, -1.0f);
        const bool truetype = sub == "TrueType";
        const Obj* enc = find(dict, "Encoding");
        std::string base_enc;
        const Obj* diffs = nullptr;
        if (enc && enc->t == Obj::Name) {
            base_enc = enc->s;
        } else if (enc && enc->t == Obj::Dict) {
            if (const Obj* be = find(*enc, "BaseEncoding"); be && be->t == Obj::Name) base_enc = be->s;
            diffs = find(*enc, "Differences");
        }
        const bool symbolic = desc && desc->t == Obj::Dict && find(*desc, "Flags") && (find(*desc, "Flags")->as_int() & 4) && base_enc.empty();
        for (int c = 32; c < 256; ++c) {
            unsigned cp = static_cast<unsigned>(c);
            if (c == 127) continue;
            if (c >= 128 && c < 160) cp = kCp1252[c - 128];
            if (!truetype && base_enc.empty() && c >= 128) {   // the standard encoding's high half
                static const struct {
                    int c;
                    unsigned cp;
                } high[] = {{161, 0xA1}, {162, 0xA2}, {163, 0xA3}, {165, 0xA5}, {167, 0xA7}, {170, 0x201C}, {171, 0xAB}, {174, 0xFB01}, {175, 0xFB02}, {177, 0x2013},
                            {178, 0x2020}, {179, 0x2021}, {180, 0xB7}, {182, 0xB6}, {183, 0x2022}, {184, 0x201A}, {185, 0x201E}, {186, 0x201D}, {187, 0xBB},
                            {188, 0x2026}, {191, 0xBF}, {208, 0x2014}};
                cp = 0;
                for (const auto& h : high)
                    if (h.c == c) cp = h.cp;
                if (!cp) continue;
            }
            put_utf8(f.simple[static_cast<std::size_t>(c)], cp);
        }
        if (diffs && diffs->t == Obj::Arr) {
            int code = 0;
            for (const Obj& x : diffs->a) {
                const Obj& r = resolve(x);
                if (r.is_num()) {
                    code = r.as_int();
                } else if (r.t == Obj::Name) {
                    if (code >= 0 && code < 256) {
                        std::string text;
                        glyph_to_utf8(r.s, text);
                        f.simple[static_cast<std::size_t>(code)] = text;
                    }
                    ++code;
                }
            }
        }
        (void)symbolic;
        f.mapped = true;
        int first = 0;
        if (const Obj* fc = find(dict, "FirstChar"); fc && fc->is_num()) first = fc->as_int();
        float scale = 1.0f;
        if (sub == "Type3") {
            if (const Obj* fm = find(dict, "FontMatrix"); fm && fm->t == Obj::Arr && !fm->a.empty()) scale = static_cast<float>(resolve(fm->a[0]).num) * 1000.0f;
        }
        if (const Obj* w = find(dict, "Widths"); w && w->t == Obj::Arr)
            for (std::size_t i = 0; i < w->a.size(); ++i) {
                const int code = first + static_cast<int>(i);
                if (code >= 0 && code < 256) f.widths[static_cast<std::size_t>(code)] = static_cast<float>(resolve(w->a[i]).num) * scale;
            }
        else if (f.mono)
            f.missing_w = 600;
    }

    // ---- one page
    struct Mat {
        double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    };
    static Mat mul(const Mat& m, const Mat& n) {   // m first, then n (row vectors)
        return {m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c, m.c * n.b + m.d * n.d, m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
    }

    struct RawRun {
        std::string text;
        float x0 = 0, x1 = 0, y = 0, size = 0;
        bool bold = false, mono = false;
        float char_w = 0;
    };

    struct GState {
        Mat ctm;
        Font* font = nullptr;
        double fs = 0, tc = 0, tw = 0, tz = 1, tl = 0;
        int render = 0;
    };

    struct PageCtx {
        std::vector<RawRun> runs;
        std::size_t bytes = 0;
    };

    Page read_page(const PageInfo& info) {
        Page page;
        const Obj* node = nullptr;
        if (info.num) {
            const Obj& n = get(info.num);
            if (n.t == Obj::Dict) node = &n;
        }
        static const Obj none;
        if (!node) node = &none;
        if (info.mediabox && info.mediabox->t == Obj::Arr && info.mediabox->a.size() >= 4) {
            const double x0 = resolve(info.mediabox->a[0]).num, y0 = resolve(info.mediabox->a[1]).num;
            const double x1 = resolve(info.mediabox->a[2]).num, y1 = resolve(info.mediabox->a[3]).num;
            if (std::fabs(x1 - x0) > 10 && std::fabs(y1 - y0) > 10) {
                page.width = static_cast<float>(std::fabs(x1 - x0));
                page.height = static_cast<float>(std::fabs(y1 - y0));
            }
        }
        PageCtx ctx;
        forms_this_page_ = 0;
        const Obj* contents = node->t == Obj::Dict ? find(*node, "Contents") : nullptr;
        std::string all;
        auto add_stream = [&](const Obj& s) {
            const Obj& r = resolve(s);
            if (!r.stream) return;
            std::string one;
            if (!decode(r, one, 64u * 1024 * 1024)) return;
            if (all.size() + one.size() > 96u * 1024 * 1024) return;
            all += one;
            all += '\n';
        };
        if (contents) {
            if (contents->t == Obj::Arr) {
                for (const Obj& s : contents->a) add_stream(s);
            } else {
                add_stream(*contents);
            }
        }
        GState gs;
        interpret(all, info.resources, gs, ctx, 0);
        page.lines = build_lines(ctx.runs);
        return page;
    }

    void interpret(const std::string& content, const Obj* resources, GState gs, PageCtx& ctx, int depth) {
        Lex lx(reinterpret_cast<const std::uint8_t*>(content.data()), content.size(), 0, true);
        std::vector<Obj> ops;
        std::vector<GState> saved;
        Mat tm, tlm;
        const Obj* fonts = resources && resources->t == Obj::Dict ? find(*resources, "Font") : nullptr;

        auto num = [&](std::size_t i) { return i < ops.size() && ops[i].is_num() ? ops[i].num : 0.0; };
        auto set_font = [&](const std::string& name, double size) {
            gs.fs = size;
            gs.font = nullptr;
            if (fonts && fonts->t == Obj::Dict)
                if (const Obj* f = find(*fonts, name.c_str())) gs.font = font_for(*f);
        };
        auto new_line = [&](double tx, double ty) {
            tlm = mul(Mat{1, 0, 0, 1, tx, ty}, tlm);
            tm = tlm;
        };
        // Shows the codes of one string (or the strings of a TJ array) as one run.
        std::string run_text;
        double run_x0 = 0, run_y = 0, run_size = 0;
        int run_glyphs = 0;
        bool run_open = false, run_bold = false, run_mono = false;
        auto origin = [&]() {
            const Mat m = mul(tm, gs.ctm);
            return std::pair<double, double>(m.e, m.f);
        };
        auto start_run = [&] {
            if (run_open) return;
            run_open = true;
            run_text.clear();
            run_glyphs = 0;
            const auto o = origin();
            run_x0 = o.first;
            run_y = o.second;
            const Mat m = mul(tm, gs.ctm);
            run_size = static_cast<double>(gs.fs) * std::hypot(m.b, m.d);
            run_bold = (gs.font && gs.font->bold) || gs.render == 2;
            run_mono = gs.font && gs.font->mono;
        };
        auto end_run = [&] {
            if (!run_open) return;
            run_open = false;
            if (run_text.empty()) return;
            RawRun r;
            r.text = run_text;
            r.x0 = static_cast<float>(run_x0);
            r.x1 = static_cast<float>(origin().first);
            r.y = static_cast<float>(run_y);
            r.size = static_cast<float>(run_size);
            r.bold = run_bold;
            r.mono = run_mono;
            r.char_w = run_glyphs > 0 ? std::fabs(r.x1 - r.x0) / static_cast<float>(run_glyphs) : 0;
            ctx.bytes += r.text.size() + sizeof(RawRun);
            if (ctx.runs.size() < 600000) ctx.runs.push_back(std::move(r));
        };
        auto advance = [&](double tx) { tm = mul(Mat{1, 0, 0, 1, tx, 0}, tm); };
        auto show = [&](const std::string& s) {
            if (!gs.font) return;
            Font& f = *gs.font;
            start_run();
            const std::size_t step = f.two_byte ? 2 : 1;
            for (std::size_t i = 0; i + step <= s.size(); i += step) {
                const std::uint32_t code = f.two_byte ? (static_cast<std::uint8_t>(s[i]) << 8) | static_cast<std::uint8_t>(s[i + 1]) : static_cast<std::uint8_t>(s[i]);
                if (gs.render != 3) {
                    const std::string& t = f.text(code);
                    run_text += t;
                    ++run_glyphs;
                }
                const double w = f.width(code) / 1000.0;
                advance((w * gs.fs + gs.tc + (!f.two_byte && code == 32 ? gs.tw : 0.0)) * gs.tz);
            }
        };

        std::size_t guard = 0;
        while (!lx.eof() && ++guard < 20000000) {
            Obj o = lx.object();
            if (o.t != Obj::Op) {
                ops.push_back(std::move(o));
                if (ops.size() > 256) ops.erase(ops.begin(), ops.begin() + 128);
                continue;
            }
            const std::string& op = o.s;
            if (op == "BT") {
                tm = tlm = Mat();
            } else if (op == "ET") {
                end_run();
            } else if (op == "Tf") {
                end_run();
                if (ops.size() >= 2 && ops[ops.size() - 2].t == Obj::Name) set_font(ops[ops.size() - 2].s, ops.back().num);
            } else if (op == "Td" || op == "TD") {
                end_run();
                if (ops.size() >= 2) {
                    const double tx = ops[ops.size() - 2].num, ty = ops.back().num;
                    if (op == "TD") gs.tl = -ty;
                    new_line(tx, ty);
                }
            } else if (op == "Tm") {
                end_run();
                if (ops.size() >= 6) {
                    const std::size_t b = ops.size() - 6;
                    tm = tlm = Mat{num(b), num(b + 1), num(b + 2), num(b + 3), num(b + 4), num(b + 5)};
                }
            } else if (op == "T*") {
                end_run();
                new_line(0, -gs.tl);
            } else if (op == "TL") {
                if (!ops.empty()) gs.tl = ops.back().num;
            } else if (op == "Tc") {
                if (!ops.empty()) gs.tc = ops.back().num;
            } else if (op == "Tw") {
                if (!ops.empty()) gs.tw = ops.back().num;
            } else if (op == "Tz") {
                if (!ops.empty()) gs.tz = ops.back().num / 100.0;
            } else if (op == "Tr") {
                if (!ops.empty()) gs.render = ops.back().as_int();
            } else if (op == "Tj") {
                if (!ops.empty() && ops.back().t == Obj::Str) show(ops.back().s);
            } else if (op == "'" || op == "\"") {
                end_run();
                if (op == "\"" && ops.size() >= 3) {
                    gs.tw = ops[ops.size() - 3].num;
                    gs.tc = ops[ops.size() - 2].num;
                }
                new_line(0, -gs.tl);
                if (!ops.empty() && ops.back().t == Obj::Str) show(ops.back().s);
            } else if (op == "TJ") {
                if (!ops.empty() && ops.back().t == Obj::Arr) {
                    for (const Obj& item : ops.back().a) {
                        if (item.t == Obj::Str) {
                            show(item.s);
                        } else if (item.is_num()) {
                            if (item.num < -180 && run_open && !run_text.empty() && run_text.back() != ' ') run_text += ' ';   // a gap this big is a space
                            advance(-item.num / 1000.0 * gs.fs * gs.tz);
                        }
                    }
                }
            } else if (op == "q") {
                saved.push_back(gs);
                if (saved.size() > 512) saved.erase(saved.begin());
            } else if (op == "Q") {
                end_run();
                if (!saved.empty()) {
                    gs = saved.back();
                    saved.pop_back();
                }
            } else if (op == "cm") {
                end_run();
                if (ops.size() >= 6) {
                    const std::size_t b = ops.size() - 6;
                    gs.ctm = mul(Mat{num(b), num(b + 1), num(b + 2), num(b + 3), num(b + 4), num(b + 5)}, gs.ctm);
                }
            } else if (op == "Do") {
                end_run();
                if (depth < 5 && !ops.empty() && ops.back().t == Obj::Name && resources && forms_this_page_ < 400) {
                    const Obj* xo = find(*resources, "XObject");
                    const Obj* x = xo && xo->t == Obj::Dict ? find(*xo, ops.back().s.c_str()) : nullptr;
                    if (x && x->stream) {
                        const Obj* st = find(*x, "Subtype");
                        if (st && st->t == Obj::Name && st->s == "Form") {
                            std::string body;
                            if (decode(*x, body, 32u * 1024 * 1024)) {
                                ++forms_this_page_;
                                GState inner = gs;
                                if (const Obj* mx = find(*x, "Matrix"); mx && mx->t == Obj::Arr && mx->a.size() >= 6)
                                    inner.ctm = mul(Mat{resolve(mx->a[0]).num, resolve(mx->a[1]).num, resolve(mx->a[2]).num, resolve(mx->a[3]).num,
                                                        resolve(mx->a[4]).num, resolve(mx->a[5]).num},
                                                    gs.ctm);
                                const Obj* res = find(*x, "Resources");
                                interpret(body, res ? res : resources, inner, ctx, depth + 1);
                            }
                        }
                    }
                }
            } else if (op == "BI") {
                // an inline picture: skip to its EI
                end_run();
                std::size_t p = lx.pos;
                const std::string_view all(content);
                const std::size_t id = all.find("ID", p);
                if (id == std::string_view::npos) break;
                p = id + 2;
                for (;;) {
                    const std::size_t ei = all.find("EI", p);
                    if (ei == std::string_view::npos) {
                        p = content.size();
                        break;
                    }
                    const bool before = ei > 0 && is_ws(static_cast<unsigned char>(content[ei - 1]));
                    const bool after = ei + 2 >= content.size() || is_ws(static_cast<unsigned char>(content[ei + 2]));
                    if (before && after) {
                        p = ei + 2;
                        break;
                    }
                    p = ei + 2;
                }
                lx.pos = p;
            }
            ops.clear();
            if (ctx.bytes > 200u * 1024 * 1024) break;
        }
        end_run();
    }

    // ---- from runs to lines
    static float run_gap_space(const RawRun& r) { return r.size * 0.16f; }

    std::vector<Line> build_lines(std::vector<RawRun>& runs) {
        // drop an exact repeat (some programs draw a word twice to make it bold)
        std::vector<RawRun> uniq;
        uniq.reserve(runs.size());
        for (RawRun& r : runs) {
            if (r.text.empty()) continue;
            if (!uniq.empty()) {
                const RawRun& p = uniq.back();
                if (p.text == r.text && std::fabs(p.x0 - r.x0) < 1.5f && std::fabs(p.y - r.y) < 1.5f) continue;
            }
            uniq.push_back(std::move(r));
        }
        struct Work {
            std::vector<RawRun> runs;
            float y = 0, size = 0, x_end = 0;
        };
        std::vector<Work> works;
        for (RawRun& r : uniq) {
            bool joined = false;
            if (!works.empty()) {
                Work& w = works.back();
                const float tol = 0.45f * std::max(w.size, r.size);
                if (std::fabs(r.y - w.y) <= tol && r.x0 >= w.x_end - 1.5f * std::max(1.0f, r.size)) {
                    w.x_end = std::max(w.x_end, r.x1);
                    w.size = std::max(w.size, r.size);
                    w.runs.push_back(std::move(r));
                    joined = true;
                }
            }
            if (!joined) {
                Work w;
                w.y = r.y;
                w.size = r.size;
                w.x_end = r.x1;
                w.runs.push_back(std::move(r));
                works.push_back(std::move(w));
            }
        }
        // is the stream order mostly top to bottom? if not, sort the lines
        std::size_t down = 0;
        for (std::size_t i = 1; i < works.size(); ++i) down += works[i].y <= works[i - 1].y + 0.5f * works[i].size;
        const bool ordered = works.size() < 2 || static_cast<double>(down) >= 0.85 * static_cast<double>(works.size() - 1);
        if (!ordered) std::stable_sort(works.begin(), works.end(), [](const Work& a, const Work& b) { return a.y > b.y + 0.3f * std::max(a.size, b.size); });

        std::vector<Line> lines;
        for (Work& w : works) {
            std::stable_sort(w.runs.begin(), w.runs.end(), [](const RawRun& a, const RawRun& b) { return a.x0 < b.x0; });
            Line line;
            double weight = 0, mono_w = 0, bold_w = 0, size_w = 0;
            float char_w = 0;
            float prev_end = 0;
            bool first = true;
            std::string text;
            std::vector<std::pair<std::size_t, const RawRun*>> spans;   // where each run's text starts in `text`
            for (const RawRun& r : w.runs) {
                std::string piece = r.text;
                // leading spaces of a piece are kept as the gap they make
                if (!first) {
                    const float gap = r.x0 - prev_end;
                    const float unit = r.mono && r.char_w > 0 ? r.char_w : run_gap_space(r) / 0.16f * 0.28f;
                    const bool ends_space = !text.empty() && text.back() == ' ';
                    const bool starts_space = !piece.empty() && piece.front() == ' ';
                    if (r.mono && r.char_w > 0) {
                        int n = static_cast<int>(std::lround(gap / unit));
                        if (n > 0 && !ends_space && !starts_space) text.append(static_cast<std::size_t>(std::min(n, 200)), ' ');
                        else if (n > 1 && (ends_space != starts_space)) text.append(static_cast<std::size_t>(std::min(n - 1, 200)), ' ');
                    } else if (gap > run_gap_space(r) && !ends_space && !starts_space) {
                        text += ' ';
                    }
                }
                first = false;
                spans.emplace_back(text.size(), &r);
                text += piece;
                prev_end = std::max(prev_end, r.x1);
                std::size_t letters = 0;
                for (char c : piece)
                    if (c != ' ') ++letters;
                const double wgt = static_cast<double>(letters);
                weight += wgt;
                mono_w += r.mono ? wgt : 0;
                bold_w += r.bold ? wgt : 0;
                size_w += wgt * static_cast<double>(r.size);
                if (r.mono && r.char_w > 0 && char_w == 0) char_w = r.char_w;
            }
            // rebuild the joined runs cleanly from the spans
            line.runs.clear();
            for (std::size_t i = 0; i < spans.size(); ++i) {
                const std::size_t from = spans[i].first;
                const std::size_t to = i + 1 < spans.size() ? spans[i + 1].first : text.size();
                const RawRun& r = *spans[i].second;
                if (!line.runs.empty() && line.runs.back().bold == r.bold && line.runs.back().mono == r.mono && std::fabs(line.runs.back().size - r.size) < 0.3f) {
                    line.runs.back().text += text.substr(from, to - from);
                } else {
                    Run run;
                    run.text = text.substr(from, to - from);
                    run.size = r.size;
                    run.bold = r.bold;
                    run.mono = r.mono;
                    line.runs.push_back(std::move(run));
                }
            }
            // trim
            std::size_t lead = 0;
            while (lead < text.size() && text[lead] == ' ') ++lead;
            std::size_t end = text.size();
            while (end > lead && text[end - 1] == ' ') --end;
            if (end == lead) continue;   // only spaces
            line.text = text.substr(lead, end - lead);
            line.y = w.y;
            line.x_end = w.x_end;
            // the first letter that is not a space gives the left edge
            line.x = w.runs.front().x0;
            for (const RawRun& r : w.runs) {
                std::size_t k = 0;
                while (k < r.text.size() && r.text[k] == ' ') ++k;
                if (k < r.text.size()) {
                    const float per = r.text.empty() ? 0 : (r.x1 - r.x0) / static_cast<float>(r.text.size());
                    line.x = r.x0 + per * static_cast<float>(k);
                    break;
                }
            }
            if (!line.runs.empty()) {
                for (Run& run : line.runs) {
                    if (&run == &line.runs.front()) {
                        std::size_t k = 0;
                        while (k < run.text.size() && run.text[k] == ' ') ++k;
                        run.text.erase(0, k);
                    }
                }
                if (!line.runs.empty() && line.runs.front().text.empty()) line.runs.erase(line.runs.begin());
                if (!line.runs.empty()) {
                    std::string& t = line.runs.back().text;
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                }
            }
            if (weight <= 0) weight = 1;
            line.size = static_cast<float>(size_w / weight);
            line.mono = mono_w >= 0.8 * weight;
            line.bold = bold_w >= 0.6 * weight;
            line.char_w = char_w;
            // a line is as big as its biggest letters when they are most of it
            float biggest = 0;
            for (const Run& r : line.runs) biggest = std::max(biggest, r.size);
            if (line.size < biggest * 0.9f && line.runs.size() == 1) line.size = biggest;
            lines.push_back(std::move(line));
        }
        return lines;
    }
};

}  // namespace

bool inflate(const unsigned char* data, std::size_t size, std::string& out, bool zlib, std::size_t limit) {
    std::size_t skip = 0;
    if (zlib) {
        if (size >= 2 && (data[0] & 0x0F) == 8 && ((data[0] << 8) | data[1]) % 31 == 0) skip = (data[1] & 0x20) ? 6 : 2;
    }
    Inflater in(data + skip, size - skip, out, limit);
    return in.run();
}

Document read_bytes(const std::string& bytes, const ProgressFn& progress) {
    Reader r(bytes, progress);
    return r.run();
}

Document read_file(const std::wstring& path, const ProgressFn& progress) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        Document d;
        d.error = "I couldn't open that file.";
        return d;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0);
    if (size <= 0 || size > 700ll * 1024 * 1024) {
        Document d;
        d.error = size <= 0 ? kNotPdf : "This PDF is too big for me to read.";
        return d;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    f.read(bytes.data(), size);
    return read_bytes(bytes, progress);
}

}  // namespace sparring::pdf

namespace sparring {

// A development helper: `cppsparring --pdf-text IN.pdf OUT.txt` writes what the reader sees, line by line.
int pdf_dump(const std::wstring& in, const std::wstring& out) {
    const pdf::Document d = pdf::read_file(in);
    std::ofstream f{std::filesystem::path(out), std::ios::binary};
    if (!d.ok) {
        f << "ERROR: " << d.error << "\n";
        return 1;
    }
    f << "title: " << d.title << "\n";
    for (std::size_t p = 0; p < d.pages.size(); ++p) {
        f << "=== page " << p + 1 << " (" << d.pages[p].width << " x " << d.pages[p].height << ")\n";
        for (const pdf::Line& l : d.pages[p].lines)
            f << "[" << static_cast<int>(l.size * 10) / 10.0 << (l.bold ? " B" : "") << (l.mono ? " M" : "") << " x=" << static_cast<int>(l.x) << " y=" << static_cast<int>(l.y) << "] " << l.text << "\n";
    }
    return 0;
}

// A development helper: `cppsparring --pdf-fuzz IN.pdf ROUNDS` damages the file in random ways and reads it, to be sure the reader
// always comes back (a crash or a hang here is a bug). Returns 0 when it got through them all.
int pdf_fuzz(const std::wstring& in, int rounds) {
    std::ifstream f(std::filesystem::path(in), std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (original.empty()) return 2;
    std::uint32_t seed = 12345;
    auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    int ok = 0;
    for (int i = 0; i < rounds; ++i) {
        std::string s = original;
        const int kind = static_cast<int>(next() % 4);
        if (kind == 0) {   // flip some bytes
            for (int k = 0, n = 1 + static_cast<int>(next() % 30); k < n; ++k) s[next() % s.size()] = static_cast<char>(next());
        } else if (kind == 1) {   // cut it off
            s.resize(next() % s.size());
        } else if (kind == 2) {   // cut a piece out
            const std::size_t a = next() % s.size();
            s.erase(a, std::min<std::size_t>(next() % 2000, s.size() - a));
        } else {   // flip bytes in the first and last parts only (the header and the tables)
            for (int k = 0; k < 10; ++k) s[(next() % 2 ? next() % std::min<std::size_t>(s.size(), 600) : s.size() - 1 - next() % std::min<std::size_t>(s.size(), 600))] = static_cast<char>(next());
        }
        const auto began = std::chrono::steady_clock::now();
        const pdf::Document d = pdf::read_bytes(s);
        ok += d.ok;
        if (std::chrono::steady_clock::now() - began > std::chrono::seconds(2)) {   // too slow: keep the file to look at
            std::error_code ec;
            const std::filesystem::path slow = std::filesystem::temp_directory_path(ec) / ("pdf-fuzz-slow-" + std::to_string(i) + ".pdf");
            std::ofstream(slow, std::ios::binary) << s;
        }
    }
    return ok >= 0 ? 0 : 1;
}

}  // namespace sparring
