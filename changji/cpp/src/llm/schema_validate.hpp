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

    // **枚举对不上不在这儿拦。**
    //
    // 这一层是用来接住"整份输出坏了"的——被 length 截断、内容过滤掐掉、
    // 压根不是 JSON。枚举填错不是那一类：它只坏一个字段，而**下游本来就
    // 按「当它没填过」处理**（storyboard.cpp 的 drop_unknown_enums，那儿
    // 写着「是抹掉不是报错：这一栏填错不值得把另外十几个好镜头一起作废」）。
    //
    // 在这儿 return 一个错，等于把那条规矩推翻：2026-09-16 实测，一集
    // 十七镜里第三镜的 face_pose 填了个表外的值，整个 /api/plan 回 400，
    // 十七镜全没了——而那个字段本来会被抹成默认值，一镜都不该丢。
    //
    // const 那一条留着：它不是"一个字段填错"，是整份输出走错了分支。
    (void)0;
    if (const auto constant = schema.find("const");
        constant != schema.end() && value != Json(*constant)) {
        return path + " 不等于 schema 规定的常量";
    }

    if (value.is_string()) {
        const std::size_t length = text::utf8_len(value.get<std::string>());
        // **写短了不在这儿拦，写空了才拦。** 理由同上面枚举那一段。
        //
        // 这些 minLength 本来就是"推一把"的数，不是地板：last_line 的下限
        // 当初还从 12 降到 4 过，理由是「"走吧。"四个字正是一集的收口」。
        // 差一两个字和"整份输出坏了"根本不是一回事，而在这儿 return 一个错
        // 的代价是整章作废——2026-09-16 实测：
        //     异步作业砸了：大模型输出不符合 chapter Schema：
        //     $.scenes[2].worse 太短
        // 写了十分钟、三场戏都在，就因为第三场的一句话短了几个字，一个字
        // 都没留下。而这些模型（走远端 API 的那些）根本不按 GBNF 生成，
        // minLength 对它们从来只是建议。
        //
        // **空串还是拦**：那不是"写短了"，是这一栏压根没写——和被截断、
        // 被内容过滤掐掉是同一类，正是这一层该接住的。
        if (const auto it = schema.find("minLength");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() > 0 && length == 0) {
            return path + " 是空的";
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
