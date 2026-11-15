#pragma once
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>
#include <cctype>
#include <algorithm>
#include <stdexcept>

namespace nlohmann {

class json {
public:
    enum class Type { Null, Boolean, Number, String, Object, Array };

private:
    Type m_type = Type::Null;
    bool m_boolVal = false;
    double m_numVal = 0.0;
    std::string m_strVal = "";
    std::map<std::string, json> m_objVal;
    std::vector<json> m_arrVal;

public:
    json() : m_type(Type::Null) {}
    json(bool v) : m_type(Type::Boolean), m_boolVal(v) {}
    json(int v) : m_type(Type::Number), m_numVal(static_cast<double>(v)) {}
    json(unsigned int v) : m_type(Type::Number), m_numVal(static_cast<double>(v)) {}
    json(long long v) : m_type(Type::Number), m_numVal(static_cast<double>(v)) {}
    json(unsigned long long v) : m_type(Type::Number), m_numVal(static_cast<double>(v)) {}
    json(double v) : m_type(Type::Number), m_numVal(v) {}
    json(const char* v) : m_type(Type::String), m_strVal(v ? v : "") {}
    json(const std::string& v) : m_type(Type::String), m_strVal(v) {}

    bool is_null() const { return m_type == Type::Null; }
    bool is_boolean() const { return m_type == Type::Boolean; }
    bool is_number() const { return m_type == Type::Number; }
    bool is_string() const { return m_type == Type::String; }
    bool is_object() const { return m_type == Type::Object; }
    bool is_array() const { return m_type == Type::Array; }

    json& operator[](const std::string& key) {
        if (m_type != Type::Object) {
            m_type = Type::Object;
            m_objVal.clear();
        }
        return m_objVal[key];
    }

    const json& operator[](const std::string& key) const {
        static const json s_null;
        if (m_type == Type::Object) {
            auto it = m_objVal.find(key);
            if (it != m_objVal.end()) return it->second;
        }
        return s_null;
    }

    bool contains(const std::string& key) const {
        if (m_type != Type::Object) return false;
        return m_objVal.find(key) != m_objVal.end();
    }

    template<typename T>
    T get() const;

    template<typename T>
    T value(const std::string& key, const T& default_val) const;

    std::string value(const std::string& key, const char* default_val) const;

    std::string dump(int indent = -1) const {
        switch (m_type) {
            case Type::Null: return "null";
            case Type::Boolean: return m_boolVal ? "true" : "false";
            case Type::Number: {
                if (m_numVal == static_cast<long long>(m_numVal))
                    return std::to_string(static_cast<long long>(m_numVal));
                return std::to_string(m_numVal);
            }
            case Type::String: {
                std::string s = "\"";
                for (char c : m_strVal) {
                    if (c == '"') s += "\\\"";
                    else if (c == '\\') s += "\\\\";
                    else if (c == '\n') s += "\\n";
                    else if (c == '\r') s += "\\r";
                    else if (c == '\t') s += "\\t";
                    else s += c;
                }
                s += "\"";
                return s;
            }
            case Type::Array: {
                std::string s = "[";
                for (size_t i = 0; i < m_arrVal.size(); ++i) {
                    if (i > 0) s += ",";
                    s += m_arrVal[i].dump();
                }
                s += "]";
                return s;
            }
            case Type::Object: {
                std::string s = "{";
                bool first = true;
                for (const auto& kv : m_objVal) {
                    if (!first) s += ",";
                    first = false;
                    s += "\"" + kv.first + "\":" + kv.second.dump();
                }
                s += "}";
                return s;
            }
        }
        return "null";
    }

    static json parse(const std::string& str) {
        size_t idx = 0;
        skip_ws(str, idx);
        return parse_val(str, idx);
    }

private:
    static void skip_ws(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
            ++i;
    }

    static json parse_val(const std::string& s, size_t& i) {
        skip_ws(s, i);
        if (i >= s.size()) return json();

        char c = s[i];
        if (c == '{') return parse_obj(s, i);
        if (c == '[') return parse_arr(s, i);
        if (c == '"') return parse_str(s, i);
        if (c == 't' || c == 'f') return parse_bool(s, i);
        if (c == 'n') return parse_null(s, i);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_num(s, i);
        return json();
    }

    static json parse_obj(const std::string& s, size_t& i) {
        json res;
        res.m_type = Type::Object;
        ++i; // skip '{'
        while (i < s.size()) {
            skip_ws(s, i);
            if (i < s.size() && s[i] == '}') {
                ++i;
                return res;
            }
            json key = parse_val(s, i);
            skip_ws(s, i);
            if (i < s.size() && s[i] == ':') ++i;
            json val = parse_val(s, i);
            res.m_objVal[key.m_strVal] = std::move(val);
            skip_ws(s, i);
            if (i < s.size() && s[i] == ',') ++i;
        }
        return res;
    }

    static json parse_arr(const std::string& s, size_t& i) {
        json res;
        res.m_type = Type::Array;
        ++i; // skip '['
        while (i < s.size()) {
            skip_ws(s, i);
            if (i < s.size() && s[i] == ']') {
                ++i;
                return res;
            }
            res.m_arrVal.push_back(parse_val(s, i));
            skip_ws(s, i);
            if (i < s.size() && s[i] == ',') ++i;
        }
        return res;
    }

    static json parse_str(const std::string& s, size_t& i) {
        ++i; // skip initial '"'
        std::string res;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\' && i < s.size()) {
                char esc = s[i++];
                if (esc == '"') res += '"';
                else if (esc == '\\') res += '\\';
                else if (esc == 'n') res += '\n';
                else if (esc == 'r') res += '\r';
                else if (esc == 't') res += '\t';
                else res += esc;
            } else {
                res += c;
            }
        }
        return json(res);
    }

    static json parse_num(const std::string& s, size_t& i) {
        size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        try {
            double val = std::stod(s.substr(start, i - start));
            return json(val);
        } catch (...) {
            return json(0.0);
        }
    }

    static json parse_bool(const std::string& s, size_t& i) {
        if (s.substr(i, 4) == "true") {
            i += 4;
            return json(true);
        }
        if (s.substr(i, 5) == "false") {
            i += 5;
            return json(false);
        }
        return json(false);
    }

    static json parse_null(const std::string& s, size_t& i) {
        if (s.substr(i, 4) == "null") i += 4;
        return json();
    }
};

template<> inline std::string json::get<std::string>() const {
    if (m_type == Type::String) return m_strVal;
    if (m_type == Type::Number) return std::to_string(static_cast<long long>(m_numVal));
    if (m_type == Type::Boolean) return m_boolVal ? "true" : "false";
    return "";
}

template<> inline int json::get<int>() const {
    if (m_type == Type::Number) return static_cast<int>(m_numVal);
    if (m_type == Type::String) {
        try { return std::stoi(m_strVal); } catch (...) {}
    }
    return 0;
}

template<> inline unsigned int json::get<unsigned int>() const {
    return static_cast<unsigned int>(get<int>());
}

template<> inline long long json::get<long long>() const {
    if (m_type == Type::Number) return static_cast<long long>(m_numVal);
    if (m_type == Type::String) {
        try { return std::stoll(m_strVal); } catch (...) {}
    }
    return 0;
}

template<> inline bool json::get<bool>() const {
    if (m_type == Type::Boolean) return m_boolVal;
    if (m_type == Type::Number) return m_numVal != 0;
    if (m_type == Type::String) return m_strVal == "true" || m_strVal == "1";
    return false;
}

template<typename T>
inline T json::value(const std::string& key, const T& default_val) const {
    if (!contains(key)) return default_val;
    try {
        return (*this)[key].get<T>();
    } catch (...) {
        return default_val;
    }
}

inline std::string json::value(const std::string& key, const char* default_val) const {
    if (!contains(key)) return default_val ? default_val : "";
    try {
        return (*this)[key].get<std::string>();
    } catch (...) {
        return default_val ? default_val : "";
    }
}

} // namespace nlohmann
