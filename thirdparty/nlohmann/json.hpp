// Minimal nlohmann/json-compatible single-header JSON library.
// Implements the subset used by this project: parse, dump, object/array
// access, type checks and implicit conversions.
#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <cstdio>
#include <sstream>
#include <cctype>
#include <istream>
#include <ostream>
#include <type_traits>

namespace nlohmann {

class json {
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };

    // ---- constructors ----
    json() : m_type(Type::Null) {}
    json(std::nullptr_t) : m_type(Type::Null) {}
    json(bool v) : m_type(Type::Boolean), m_bool(v) {}
    json(int v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    json(unsigned int v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    json(long v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    json(long long v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    json(unsigned long v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    json(unsigned long long v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    json(float v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    json(double v) : m_type(Type::Number), m_number(v) {}
    json(const char* v) : m_type(Type::String), m_str(v) {}
    json(const std::string& v) : m_type(Type::String), m_str(v) {}
    json(std::string&& v) : m_type(Type::String), m_str(std::move(v)) {}
    json(const std::vector<json>& v) : m_type(Type::Array), m_arr(new std::vector<json>(v)) {}
    json(const std::map<std::string, json>& v) : m_type(Type::Object), m_obj(new std::map<std::string, json>(v)) {}
    json(const std::vector<std::string>& v) : m_type(Type::Array), m_arr(new std::vector<json>()) {
        for (const auto& s : v) m_arr->push_back(json(s));
    }
    json(const std::vector<long long>& v) : m_type(Type::Array), m_arr(new std::vector<json>()) {
        for (const auto& n : v) m_arr->push_back(json(n));
    }

    // 初始化列表构造：所有元素都是 [string, value] 二元数组时构造 object，
    // 否则构造 array（与 nlohmann 官方库行为一致）
    json(std::initializer_list<json> init) {
        bool isObject = !init.empty();
        for (const auto& e : init) {
            // 用 m_arr 直接访问，避免 0 作为空指针常量与 operator[](size_t) 产生歧义
            if (!(e.is_array() && e.size() == 2 && e.m_arr->at(0).is_string())) { isObject = false; break; }
        }
        if (isObject) {
            m_type = Type::Object;
            m_obj.reset(new std::map<std::string, json>());
            for (const auto& e : init) (*m_obj)[e.m_arr->at(0).m_str] = std::move(e.m_arr->at(1));
        } else {
            m_type = Type::Array;
            m_arr.reset(new std::vector<json>());
            for (const auto& e : init) m_arr->push_back(e);
        }
    }

    // ---- 拷贝 / 移动语义（成员含 unique_ptr，需显式深拷贝）----
    json(const json& o)
        : m_type(o.m_type), m_bool(o.m_bool), m_number(o.m_number), m_str(o.m_str) {
        if (o.m_arr) m_arr.reset(new std::vector<json>(*o.m_arr));
        if (o.m_obj) m_obj.reset(new std::map<std::string, json>(*o.m_obj));
    }
    json(json&& o) noexcept
        : m_type(o.m_type), m_bool(o.m_bool), m_number(o.m_number),
          m_str(std::move(o.m_str)), m_arr(std::move(o.m_arr)), m_obj(std::move(o.m_obj)) {
        o.m_type = Type::Null;
    }
    json& operator=(const json& o) {
        if (this != &o) { json tmp(o); *this = std::move(tmp); }
        return *this;
    }
    json& operator=(json&& o) noexcept {
        if (this != &o) {
            m_type = o.m_type; m_bool = o.m_bool; m_number = o.m_number;
            m_str = std::move(o.m_str); m_arr = std::move(o.m_arr); m_obj = std::move(o.m_obj);
            o.m_type = Type::Null;
        }
        return *this;
    }

    // 隐式转换运算符（兼容 nlohmann 官方库的用法习惯）
    operator bool() const { return get<bool>(); }
    operator int() const { return get<int>(); }
    operator long long() const { return get<long long>(); }
    operator double() const { return get<double>(); }
    operator std::string() const { return get<std::string>(); }

    static json array() { json j; j.m_type = Type::Array; j.m_arr.reset(new std::vector<json>()); return j; }
    static json object() { json j; j.m_type = Type::Object; j.m_obj.reset(new std::map<std::string, json>()); return j; }

    Type type() const { return m_type; }
    bool is_null() const { return m_type == Type::Null; }
    bool is_boolean() const { return m_type == Type::Boolean; }
    bool is_number() const { return m_type == Type::Number; }
    bool is_string() const { return m_type == Type::String; }
    bool is_array() const { return m_type == Type::Array; }
    bool is_object() const { return m_type == Type::Object; }
    // legacy names kept for compatibility with earlier skeleton code
    bool is_bool() const { return is_boolean(); }

    // ---- array / object access ----
    json& operator[](size_t index) {
        ensureArray();
        if (index >= m_arr->size()) m_arr->resize(index + 1);
        return (*m_arr)[index];
    }
    const json& operator[](size_t index) const {
        static const json s_null;
        if (m_type != Type::Array || index >= m_arr->size()) return s_null;
        return (*m_arr)[index];
    }

    json& operator[](const std::string& key) {
        ensureObject();
        return (*m_obj)[key];
    }
    const json& operator[](const std::string& key) const {
        static const json s_null;
        if (m_type != Type::Object) return s_null;
        auto it = m_obj->find(key);
        return it != m_obj->end() ? it->second : s_null;
    }

    // const char* 重载：避免字符串字面量在隐式转换运算符存在时
    // 与 operator[](size_t) 产生 bool→整型的标准转换歧义
    json& operator[](const char* key) { return (*this)[std::string(key)]; }
    const json& operator[](const char* key) const { return (*this)[std::string(key)]; }

    bool contains(const std::string& key) const {
        return m_type == Type::Object && m_obj->count(key) > 0;
    }

    template<typename T>
    T value(const std::string& key, const T& defaultValue) const {
        return contains(key) ? (*this)[key].get<T>() : defaultValue;
    }

    void push_back(json v) {
        ensureArray();
        m_arr->push_back(std::move(v));
    }

    bool empty() const { return size() == 0; }

    size_t size() const {
        if (m_type == Type::Array) return m_arr->size();
        if (m_type == Type::Object) return m_obj->size();
        return 0;
    }

    // iteration support (arrays)
    std::vector<json>::iterator begin() { return m_arr->begin(); }
    std::vector<json>::iterator end() { return m_arr->end(); }
    std::vector<json>::const_iterator begin() const { return m_arr->begin(); }
    std::vector<json>::const_iterator end() const { return m_arr->end(); }

    // ---- conversions ----
    // 单一定义 + if constexpr 分发，避免显式特化的实例化顺序问题
    template<typename T> T get() const {
        if constexpr (std::is_same_v<T, json>) {
            return *this;   // json -> json 透传
        } else if constexpr (std::is_same_v<T, bool>) {
            if (m_type == Type::Boolean) return m_bool;
            if (m_type == Type::Number) return m_number != 0;
            if (m_type == Type::String) return m_str == "true" || m_str == "1";
            return false;
        } else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, short> ||
                             std::is_same_v<T, long> || std::is_same_v<T, unsigned int>) {
            if (m_type == Type::Number) return static_cast<T>(m_number);
            if (m_type == Type::Boolean) return m_bool ? 1 : 0;
            if (m_type == Type::String) return static_cast<T>(std::atoi(m_str.c_str()));
            return 0;
        } else if constexpr (std::is_same_v<T, long long> || std::is_same_v<T, unsigned long long> ||
                             std::is_same_v<T, unsigned long>) {
            if (m_type == Type::Number) return static_cast<T>(m_number);
            if (m_type == Type::Boolean) return m_bool ? 1 : 0;
            if (m_type == Type::String) return static_cast<T>(std::atoll(m_str.c_str()));
            return 0;
        } else if constexpr (std::is_floating_point_v<T>) {
            if (m_type == Type::Number) return static_cast<T>(m_number);
            if (m_type == Type::String) return static_cast<T>(std::atof(m_str.c_str()));
            return 0;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (m_type == Type::String) return m_str;
            if (m_type == Type::Boolean) return m_bool ? "true" : "false";
            if (m_type == Type::Number) { std::ostringstream oss; oss << m_number; return oss.str(); }
            return "";
        } else {
            static_assert(!sizeof(T), "json::get<T> unsupported type");
            return T();
        }
    }

    std::string get_string() const { return get<std::string>(); }
    int get_int() const { return get<int>(); }
    bool get_bool() const { return get<bool>(); }
    double get_double() const { return get<double>(); }

    // ---- serialization ----
    std::string dump(int indent = -1) const {
        std::string out;
        dumpTo(out, indent, 0);
        return out;
    }

    // ---- parsing ----
    class json_parse_error : public std::runtime_error {
    public: using std::runtime_error::runtime_error;
    };

    static json parse(const std::string& text) {
        size_t pos = 0;
        json j = parseValue(text, pos);
        skipWs(text, pos);
        if (pos != text.size()) throw json_parse_error("Unexpected trailing characters at offset " + std::to_string(pos));
        return j;
    }

private:
    void ensureArray() {
        if (m_type == Type::Null) { m_type = Type::Array; m_arr.reset(new std::vector<json>()); }
        else if (m_type != Type::Array) throw std::runtime_error("json: not an array");
    }
    void ensureObject() {
        if (m_type == Type::Null) { m_type = Type::Object; m_obj.reset(new std::map<std::string, json>()); }
        else if (m_type != Type::Object) throw std::runtime_error("json: not an object");
    }

    static void skipWs(const std::string& s, size_t& pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) ++pos;
    }

    static json parseValue(const std::string& s, size_t& pos) {
        skipWs(s, pos);
        if (pos >= s.size()) throw json_parse_error("Unexpected end of input");
        char c = s[pos];
        if (c == '{') return parseObject(s, pos);
        if (c == '[') return parseArray(s, pos);
        if (c == '"') return json(parseString(s, pos));
        if (c == 't') { expect(s, pos, "true"); return json(true); }
        if (c == 'f') { expect(s, pos, "false"); return json(false); }
        if (c == 'n') { expect(s, pos, "null"); return json(); }
        return parseNumber(s, pos);
    }

    static void expect(const std::string& s, size_t& pos, const char* lit) {
        size_t len = std::string(lit).size();
        if (s.compare(pos, len, lit) != 0) throw json_parse_error(std::string("Expected '") + lit + "'");
        pos += len;
    }

    static json parseNumber(const std::string& s, size_t& pos) {
        size_t start = pos;
        if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
        while (pos < s.size() && (isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' || s[pos] == '-' || s[pos] == '+')) ++pos;
        std::string num = s.substr(start, pos - start);
        if (num.empty()) throw json_parse_error("Invalid number at offset " + std::to_string(start));
        return json(std::stod(num));
    }

    static std::string parseString(const std::string& s, size_t& pos) {
        if (s[pos] != '"') throw json_parse_error("Expected string");
        ++pos;
        std::string out;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos >= s.size()) break;
                char e = s[pos++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (pos + 4 > s.size()) throw json_parse_error("Invalid \\u escape");
                        unsigned code = static_cast<unsigned>(std::stoul(s.substr(pos, 4), nullptr, 16));
                        pos += 4;
                        // surrogate pair
                        if (code >= 0xD800 && code <= 0xDBFF && pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                            unsigned low = static_cast<unsigned>(std::stoul(s.substr(pos + 2, 4), nullptr, 16));
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                                pos += 6;
                            }
                        }
                        encodeUtf8(out, code);
                        break;
                    }
                    default: throw json_parse_error(std::string("Invalid escape '\\") + e + "'");
                }
            } else {
                out += c;
            }
        }
        throw json_parse_error("Unterminated string");
    }

    static void encodeUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
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

    static json parseArray(const std::string& s, size_t& pos) {
        ++pos; // '['
        json arr = array();
        skipWs(s, pos);
        if (pos < s.size() && s[pos] == ']') { ++pos; return arr; }
        while (true) {
            arr.m_arr->push_back(parseValue(s, pos));
            skipWs(s, pos);
            if (pos >= s.size()) throw json_parse_error("Unterminated array");
            if (s[pos] == ',') { ++pos; continue; }
            if (s[pos] == ']') { ++pos; return arr; }
            throw json_parse_error("Expected ',' or ']' in array");
        }
    }

    static json parseObject(const std::string& s, size_t& pos) {
        ++pos; // '{'
        json obj = object();
        skipWs(s, pos);
        if (pos < s.size() && s[pos] == '}') { ++pos; return obj; }
        while (true) {
            skipWs(s, pos);
            std::string key = parseString(s, pos);
            skipWs(s, pos);
            if (pos >= s.size() || s[pos] != ':') throw json_parse_error("Expected ':' in object");
            ++pos;
            (*obj.m_obj)[key] = parseValue(s, pos);
            skipWs(s, pos);
            if (pos >= s.size()) throw json_parse_error("Unterminated object");
            if (s[pos] == ',') { ++pos; continue; }
            if (s[pos] == '}') { ++pos; return obj; }
            throw json_parse_error("Expected ',' or '}' in object");
        }
    }

    void dumpTo(std::string& out, int indent, int depth) const {
        std::string pad = indent >= 0 ? std::string(static_cast<size_t>(indent) * (depth + 1), ' ') : "";
        std::string padEnd = indent >= 0 ? std::string(static_cast<size_t>(indent) * depth, ' ') : "";
        const char* nl = indent >= 0 ? "\n" : "";
        const char* sep = indent >= 0 ? ": " : ":";
        switch (m_type) {
            case Type::Null: out += "null"; break;
            case Type::Boolean: out += m_bool ? "true" : "false"; break;
            case Type::Number: dumpNumber(out); break;
            case Type::String: dumpString(out, m_str); break;
            case Type::Array: {
                out += "[";
                if (!m_arr->empty()) {
                    for (size_t i = 0; i < m_arr->size(); ++i) {
                        out += nl + pad;
                        (*m_arr)[i].dumpTo(out, indent, depth + 1);
                        if (i + 1 < m_arr->size()) out += ",";
                    }
                    out += nl + padEnd;
                }
                out += "]";
                break;
            }
            case Type::Object: {
                out += "{";
                if (!m_obj->empty()) {
                    size_t i = 0;
                    for (const auto& kv : *m_obj) {
                        out += nl + pad;
                        dumpString(out, kv.first);
                        out += sep;
                        kv.second.dumpTo(out, indent, depth + 1);
                        if (++i < m_obj->size()) out += ",";
                    }
                    out += nl + padEnd;
                }
                out += "}";
                break;
            }
        }
    }

    void dumpNumber(std::string& out) const {
        if (std::isfinite(m_number) && m_number == std::floor(m_number) &&
            std::fabs(m_number) < 9.0e15) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_number));
            out += buf;
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.17g", m_number);
            out += buf;
        }
    }

    static void dumpString(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_str;
    std::unique_ptr<std::vector<json>> m_arr;
    std::unique_ptr<std::map<std::string, json>> m_obj;
};

// stream helpers
inline std::ostream& operator<<(std::ostream& os, const json& j) { return os << j.dump(); }
inline std::istream& operator>>(std::istream& is, json& j) {
    std::string content((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    j = json::parse(content);
    return is;
}

} // namespace nlohmann
