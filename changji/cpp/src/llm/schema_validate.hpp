#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "stages/json_extract.hpp"
#include "util/text.hpp"

namespace changji::llm {
namespace detail {

using Json = nlohmann::json;
using Schema = nlohmann::ordered_json;

inline const Schema* resolve_ref(const Schema& root, const std::string& ref) {
    constexpr const char* prefix = "#/$defs/";
    if (ref.rfind(prefix, 0) != 0) return nullptr;
    const std::string key = ref.substr(std::char_traits<char>::length(prefix));
    const auto defs = root.find("$defs");
    if (defs == root.end() || !defs->is_object()) return nullptr;
    const auto it = defs->find(key);
    return it == defs->end() ? nullptr : &*it;
}

inline bool type_matches(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return true;
}

inline std::optional<std::string> validate_value(
    const Json& value, const Schema& schema, const Schema& root,
    const std::string& path) {
    if (const auto ref = schema.find("$ref"); ref != schema.end() && ref->is_string()) {
        const Schema* target = resolve_ref(root, ref->get<std::string>());
        if (target == nullptr) return path + " 使用了无法解析的 $ref";
        return validate_value(value, *target, root, path);
    }

    if (const auto choices = schema.find("anyOf");
        choices != schema.end() && choices->is_array()) {
        for (const auto& choice : *choices) {
            if (!validate_value(value, choice, root, path).has_value()) return std::nullopt;
        }
        return path + " 不符合 anyOf 中的任何一种结构";
    }

    if (const auto choices = schema.find("oneOf");
        choices != schema.end() && choices->is_array()) {
        int matched = 0;
        for (const auto& choice : *choices) {
            if (!validate_value(value, choice, root, path).has_value()) ++matched;
        }
        if (matched != 1) return path + " 不符合 oneOf 的唯一一种结构";
    }

    if (const auto type = schema.find("type"); type != schema.end()) {
        bool matched = true;
        if (type->is_string()) {
            matched = type_matches(value, type->get<std::string>());
        } else if (type->is_array()) {
            matched = false;
            for (const auto& one : *type) {
                if (one.is_string() && type_matches(value, one.get<std::string>())) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) return path + " 的类型不符合 schema";
    }

    if (const auto values = schema.find("enum");
        values != schema.end() && values->is_array()) {
        bool found = false;
        for (const auto& allowed : *values) {
            if (value == Json(allowed)) {
                found = true;
                break;
            }
        }
        if (!found) return path + " 不在允许的枚举值中";
    }
    if (const auto constant = schema.find("const");
        constant != schema.end() && value != Json(*constant)) {
        return path + " 不等于 schema 规定的常量";
    }

    if (value.is_string()) {
        const std::size_t length = text::utf8_len(value.get<std::string>());
        if (const auto it = schema.find("minLength");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() >= 0 &&
            length < static_cast<std::size_t>(it->get<long long>())) {
            return path + " 太短";
        }
        if (const auto it = schema.find("maxLength");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() >= 0 &&
            length > static_cast<std::size_t>(it->get<long long>())) {
            return path + " 太长";
        }
    }

    if (value.is_array()) {
        if (const auto it = schema.find("minItems");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() >= 0 &&
            value.size() < static_cast<std::size_t>(it->get<long long>())) {
            return path + " 的项目太少";
        }
        if (const auto it = schema.find("maxItems");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() >= 0 &&
            value.size() > static_cast<std::size_t>(it->get<long long>())) {
            return path + " 的项目太多";
        }
        if (const auto items = schema.find("items");
            items != schema.end() && items->is_object()) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (auto err = validate_value(value[i], *items, root,
                                              path + "[" + std::to_string(i) + "]")) {
                    return err;
                }
            }
        }
    }

    if (value.is_object()) {
        if (const auto required = schema.find("required");
            required != schema.end() && required->is_array()) {
            for (const auto& key : *required) {
                if (key.is_string() && !value.contains(key.get<std::string>())) {
                    return path + " 缺少必填字段 " + key.get<std::string>();
                }
            }
        }
        const auto properties = schema.find("properties");
        if (properties != schema.end() && properties->is_object()) {
            for (const auto& item : properties->items()) {
                const auto actual = value.find(item.key());
                if (actual == value.end()) continue;
                if (auto err = validate_value(*actual, item.value(), root,
                                              path + "." + item.key())) {
                    return err;
                }
            }
            const auto additional = schema.find("additionalProperties");
            if (additional != schema.end() && additional->is_boolean() &&
                !additional->get<bool>()) {
                for (const auto& item : value.items()) {
                    if (!properties->contains(item.key())) {
                        return path + " 含有未声明字段 " + item.key();
                    }
                }
            }
        }
    }

    return std::nullopt;
}

}  // namespace detail

inline std::optional<std::string> validate_json_schema(
    const nlohmann::json& value, const nlohmann::ordered_json& schema) {
    if (schema.is_null() || schema.empty()) return std::nullopt;
    return detail::validate_value(value, schema, schema, "$");
}

inline std::optional<std::string> validate_structured_output(
    const std::string& raw, const nlohmann::ordered_json& schema) {
    if (schema.is_null() || schema.empty()) return std::nullopt;
    try {
        const nlohmann::json value = stages::extract_json(raw);
        return validate_json_schema(value, schema);
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
}

}  // namespace changji::llm
