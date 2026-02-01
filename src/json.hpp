#pragma once

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace simple_json {

class Value {
public:
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;

    Value() : data_(nullptr) {}
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool value) : data_(value) {}
    Value(double value) : data_(value) {}
    Value(std::string value) : data_(std::move(value)) {}
    Value(Object value) : data_(std::move(value)) {}
    Value(Array value) : data_(std::move(value)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data_); }
    bool is_bool() const { return std::holds_alternative<bool>(data_); }
    bool is_number() const { return std::holds_alternative<double>(data_); }
    bool is_string() const { return std::holds_alternative<std::string>(data_); }
    bool is_object() const { return std::holds_alternative<Object>(data_); }
    bool is_array() const { return std::holds_alternative<Array>(data_); }

    const Object& as_object() const { return std::get<Object>(data_); }
    const Array& as_array() const { return std::get<Array>(data_); }
    const std::string& as_string() const { return std::get<std::string>(data_); }
    double as_number() const { return std::get<double>(data_); }
    bool as_bool() const { return std::get<bool>(data_); }

    const Value& at(const std::string& key) const {
        const auto& obj = as_object();
        auto it = obj.find(key);
        if (it == obj.end()) {
            throw std::runtime_error("Missing key: " + key);
        }
        return it->second;
    }

    const Value* find(const std::string& key) const {
        if (!is_object()) {
            return nullptr;
        }
        const auto& obj = as_object();
        auto it = obj.find(key);
        if (it == obj.end()) {
            return nullptr;
        }
        return &it->second;
    }

private:
    std::variant<std::nullptr_t, bool, double, std::string, Object, Array> data_;
};

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input), pos_(0) {}

    Value parse() {
        skip_ws();
        Value value = parse_value();
        skip_ws();
        if (pos_ != input_.size()) {
            throw std::runtime_error("Unexpected trailing data");
        }
        return value;
    }

private:
    Value parse_value() {
        if (match('n')) {
            expect("ull");
            return Value(nullptr);
        }
        if (match('t')) {
            expect("rue");
            return Value(true);
        }
        if (match('f')) {
            expect("alse");
            return Value(false);
        }
        if (peek() == '"') {
            return Value(parse_string());
        }
        if (peek() == '{') {
            return Value(parse_object());
        }
        if (peek() == '[') {
            return Value(parse_array());
        }
        if (peek() == '-' || std::isdigit(peek())) {
            return Value(parse_number());
        }
        throw std::runtime_error("Invalid JSON value");
    }

    Value::Object parse_object() {
        expect('{');
        skip_ws();
        Value::Object obj;
        if (peek() == '}') {
            pos_++;
            return obj;
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            obj.emplace(std::move(key), parse_value());
            skip_ws();
            if (match('}')) {
                break;
            }
            expect(',');
        }
        return obj;
    }

    Value::Array parse_array() {
        expect('[');
        skip_ws();
        Value::Array array;
        if (peek() == ']') {
            pos_++;
            return array;
        }
        while (true) {
            skip_ws();
            array.push_back(parse_value());
            skip_ws();
            if (match(']')) {
                break;
            }
            expect(',');
        }
        return array;
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (pos_ < input_.size()) {
            char c = input_[pos_++];
            if (c == '"') {
                return result;
            }
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    throw std::runtime_error("Invalid escape");
                }
                char esc = input_[pos_++];
                switch (esc) {
                    case '"':
                    case '\\':
                    case '/':
                        result.push_back(esc);
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    case 'u':
                        result += parse_unicode();
                        break;
                    default:
                        throw std::runtime_error("Invalid escape");
                }
            } else {
                result.push_back(c);
            }
        }
        throw std::runtime_error("Unterminated string");
    }

    std::string parse_unicode() {
        if (pos_ + 4 > input_.size()) {
            throw std::runtime_error("Invalid unicode escape");
        }
        unsigned int code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = input_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code += static_cast<unsigned int>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code += static_cast<unsigned int>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code += static_cast<unsigned int>(c - 'A' + 10);
            } else {
                throw std::runtime_error("Invalid unicode escape");
            }
        }
        std::string result;
        if (code <= 0x7F) {
            result.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return result;
    }

    double parse_number() {
        size_t start = pos_;
        if (peek() == '-') {
            pos_++;
        }
        while (std::isdigit(peek())) {
            pos_++;
        }
        if (peek() == '.') {
            pos_++;
            while (std::isdigit(peek())) {
                pos_++;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            pos_++;
            if (peek() == '+' || peek() == '-') {
                pos_++;
            }
            while (std::isdigit(peek())) {
                pos_++;
            }
        }
        return std::stod(std::string(input_.substr(start, pos_ - start)));
    }

    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            pos_++;
        }
    }

    char peek() const {
        if (pos_ >= input_.size()) {
            return '\0';
        }
        return input_[pos_];
    }

    bool match(char c) {
        if (peek() == c) {
            pos_++;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!match(c)) {
            throw std::runtime_error("Expected character");
        }
    }

    void expect(std::string_view expected) {
        for (char c : expected) {
            expect(c);
        }
    }

    std::string_view input_;
    size_t pos_;
};

inline Value parse(std::string_view input) {
    return Parser(input).parse();
}

}  // namespace simple_json
