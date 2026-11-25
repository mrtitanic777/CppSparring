#pragma once
// A small JSON reader: enough for Clang's AST dump and the lesson files.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sparring::json {

enum class JType { Null, String, Number, Bool, Array, Object };

struct JValue {
    JType type = JType::Null;
    std::string str;
    double num = 0;
    bool boolean = false;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;

    const JValue* find(const char* key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string text(const char* key) const {
        const JValue* v = find(key);
        return v && v->type == JType::String ? v->str : std::string();
    }
};

class JParser {
public:
    explicit JParser(const std::string& s) : p_(s.c_str()), end_(s.c_str() + s.size()) {}

    JValue parse() {
        ws();
        if (p_ >= end_) return {};
        switch (*p_) {
        case '"': return string();
        case '{': return object();
        case '[': return array();
        case 't':
        case 'f': return boolean();
        case 'n': p_ = std::min(end_, p_ + 4); return {};
        default: return number();
        }
    }

private:
    const char* p_;
    const char* end_;

    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
    }
    static void utf8(std::string& out, unsigned cp) {
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
    unsigned hex4() {
        if (end_ - p_ < 5) return '?';
        const std::string h(p_ + 1, p_ + 5);
        p_ += 4;
        return static_cast<unsigned>(std::strtoul(h.c_str(), nullptr, 16));
    }
    JValue string() {
        JValue v;
        v.type = JType::String;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ == '\\' && p_ + 1 < end_) {
                ++p_;
                switch (*p_) {
                case 'n': v.str += '\n'; break;
                case 't': v.str += '\t'; break;
                case 'r': break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ > 6 && p_[1] == '\\' && p_[2] == 'u') {
                        ++p_;
                        const unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(v.str, cp);
                    break;
                }
                default: v.str += *p_; break;
                }
            } else {
                v.str += *p_;
            }
            ++p_;
        }
        if (p_ < end_) ++p_;
        return v;
    }
    JValue number() {
        JValue v;
        v.type = JType::Number;
        char* stop = nullptr;
        v.num = std::strtod(p_, &stop);
        p_ = stop && stop > p_ ? stop : p_ + 1;
        return v;
    }
    JValue boolean() {
        JValue v;
        v.type = JType::Bool;
        v.boolean = *p_ == 't';
        p_ = std::min(end_, p_ + (v.boolean ? 4 : 5));
        return v;
    }
    JValue array() {
        JValue v;
        v.type = JType::Array;
        ++p_;
        ws();
        while (p_ < end_ && *p_ != ']') {
            v.arr.push_back(parse());
            ws();
            if (p_ < end_ && *p_ == ',') ++p_;
            ws();
        }
        if (p_ < end_) ++p_;
        return v;
    }
    JValue object() {
        JValue v;
        v.type = JType::Object;
        ++p_;
        ws();
        while (p_ < end_ && *p_ != '}') {
            std::string key = string().str;
            ws();
            if (p_ < end_ && *p_ == ':') ++p_;
            v.obj.emplace_back(std::move(key), parse());
            ws();
            if (p_ < end_ && *p_ == ',') ++p_;
            ws();
        }
        if (p_ < end_) ++p_;
        return v;
    }
};

}  // namespace sparring::json
