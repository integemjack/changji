#pragma once

// 角色与场景资产。
//
// 外观描述只存在于这里。分镜表里只有 id。渲染提示词时由程序把不可变外观块
// 和本镜可变项机械拼接，保证同一角色在几十个镜头里拿到逐字节相同的描述。
//
// 移植自 src/changji/models/character.py。
//
// ⚠️ 这个文件里的 render_prompt 系列函数**输出必须与 Python 侧逐字节相同**。
// 它们的结果直接进提示词，差一个标点画面就会漂。方案第六节把提示词拼接
// 单独列为"必须逐字节"的一类，与接口契约的"结构兼容"标准不同。

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/json_compat.hpp"
#include "models/ordered_map.hpp"

namespace changji::models {

/// 风格线。决定用哪套出图基座和提示词范式。
enum class StyleLine { REALISTIC, ANIME };

NLOHMANN_JSON_SERIALIZE_ENUM(StyleLine, {
    {StyleLine::REALISTIC, "realistic"},
    {StyleLine::ANIME, "anime"},
})

const char* to_string(StyleLine v);

/// 不可变外观块。一旦定稿就不再改，改了等于换角色。
///
/// 五段式结构。拼提示词时按固定顺序连接，顺序也不能变，
/// 因为提示词里靠前的词权重更高，顺序变了画面就会漂。
struct AppearanceBlock {
    std::string identity; ///< 身份：性别、年龄段、气质
    std::string body;     ///< 体型、身高感
    std::string face;     ///< 五官、发型、发色、瞳色
    std::string attire;   ///< 默认服装
    std::string style;    ///< 该角色特有的画风修饰

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        AppearanceBlock, identity, body, face, attire, style)

    /// 拼成提示词片段。写实线用自然语言，动漫线用标签串。
    ///
    /// 各段的尾部标点要去掉。手写的设定里常带句号，拼接后会变成
    /// 「冷静克制。，身姿笔挺。，」这样标点重复的串，
    /// 而这个串会出现在每一个镜头的提示词里。
    std::string render(StyleLine style_line) const;
};

/// 服装变体。剧情里换装、衣服破损这些状态。
struct WardrobeVariant {
    std::string wardrobe_id;
    std::string description; ///< 覆盖 AppearanceBlock.attire

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        WardrobeVariant, wardrobe_id, description)
};

/// 一个角色。
struct Character {
    std::string char_id; ///< 必须以 c_ 开头；^c_[a-z0-9_]+$
    std::string name;    ///< 剧本里的称呼
    AppearanceBlock appearance;
    std::vector<WardrobeVariant> wardrobe;

    // 参考图，相对项目根的路径
    std::optional<std::string> ref_front;
    std::optional<std::string> ref_three_quarter;
    std::optional<std::string> ref_back;

    // 训练出来的角色 LoRA
    std::optional<std::string> lora_path;
    std::optional<std::string> lora_trigger;
    double lora_strength = 1.0; ///< 0..2

    // 配音
    std::optional<std::string> voice_id;
    std::optional<std::string> voice_ref_audio;
    /// 猜出来的性别和角色序号。配音时拿它们从服务端的音色列表里挑。
    std::string voice_gender;
    int voice_order = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Character, char_id, name, appearance, wardrobe,
        ref_front, ref_three_quarter, ref_back,
        lora_path, lora_trigger, lora_strength,
        voice_id, voice_ref_audio, voice_gender, voice_order)

    void validate(std::vector<std::string>& errs) const;

    /// 取指定服装状态的描述，找不到就退回默认。
    std::string wardrobe_desc(const std::string& wardrobe_state) const;

    /// 渲染该角色的完整外观提示词片段。
    /// 换装时只替换 attire 那一段，其余逐字节不变。
    std::string render_prompt(StyleLine style_line,
                              const std::string& wardrobe_state = "default") const;

    /// 按面部朝向挑参考图。
    std::optional<std::string> ref_for_pose(const std::string& face_pose) const;
};

/// 一个场景。空景图是场景一致性的锚点。
struct Location {
    std::string location_id; ///< ^loc_[a-z0-9_]+$
    std::string name;
    std::string space;    ///< 空间结构
    std::string lighting; ///< 光线基调，如 冷调顶光、暖调侧逆光
    std::string palette;  ///< 色彩方案
    std::optional<std::string> ref_empty; ///< 空景图路径，无人物

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Location, location_id, name, space, lighting, palette, ref_empty)

    void validate(std::vector<std::string>& errs) const;

    std::string render_prompt(StyleLine style_line) const;
};

/// 全剧统一的风格层。所有镜头共用，保证整体调性不漂。
/// 这条线长什么样。**没有它，"写实线 / 动漫线"这个选择等于没选。**
///
/// 2026-09-12 用户报的：同一个项目里三个角色，一个是皮克斯 3D、一个半写实、
/// 一个基本是照片；空景图一样，便利店是蓝到发光的 CG，咖啡馆是实拍质感。
/// 查出来是**提示词里一个字的画风信息都没有**——`style_line` 只决定拼接用
/// 的逗号是中文还是英文，以及动漫线往负向里加一句"压写实"；写实线什么都不
/// 加。而唯一的画风字段 `global_style` 默认是空的（等 AI 写圣经时才填，
/// 很多项目根本没走那一步）。于是每张图各自发挥。
///
/// **这句话只在读资产库时用来补空**（见 ProjectStore::load_assets），
/// 补完之后它就是项目里的一条普通数据：项目页那个「画风」框里看得见、
/// 改得动。不在出图那一层偷偷加——那样用户看到的框是空的，却改不了
/// 实际生效的东西。
std::string default_style(StyleLine style_line);

struct StyleProfile {
    StyleLine style_line = StyleLine::REALISTIC;
    std::string global_style;    ///< 全剧画风、色温、质感
    std::string negative_prompt;
    std::string aspect_ratio = "9:16"; ///< 9:16 竖屏 或 16:9 横屏

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        StyleProfile, style_line, global_style, negative_prompt, aspect_ratio)
};

/// 角色和场景的资产库。分镜表里的每个 id 都必须能在这里查到。
struct AssetLibrary {
    // 必须保持插入顺序，不能用 std::map。
    //
    // 一开始用的是 std::map，理由是 character_ids() 要返回排序后的列表。
    // 但那漏了另一半：接口响应里 characters/locations 是**数组**，
    // 而 Python 的 dict 保持插入顺序——用 std::map 会按 key 排序，
    // 两个后端返回的场景列表顺序不同，前端渲染出来肉眼可见。
    // 对拍测试直接抓到了这一条（/api/assets 的 locations 顺序反了）。
    //
    // 排序的需求由 sorted_keys() 单独提供，见 ordered_map.hpp。
    OrderedMap<Character> characters;
    OrderedMap<Location> locations;
    StyleProfile style;

    // 这里**不能**用 NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT。
    //
    // 那个宏展开出来的 from_json 形参写死是 nlohmann::json&，不是模板。
    // 于是 ordered_json.get<AssetLibrary>() 会先把 ordered_json 隐式转成
    // json——而那次转换正好把键排序了，顺序在进 from_json 之前就丢光。
    // 在 load_assets 里换 ordered_json 完全不起作用，症状是接口响应里
    // locations 数组变成字典序。对拍测试抓到的就是这一条。
    //
    // 所以手写，两个 JSON 类型各给一个显式重载，实现共用下面的模板。
    friend void to_json(nlohmann::json& j, const AssetLibrary& t);
    friend void from_json(const nlohmann::json& j, AssetLibrary& t);
    friend void to_json(nlohmann::ordered_json& j, const AssetLibrary& t);
    friend void from_json(const nlohmann::ordered_json& j, AssetLibrary& t);

    /// 给大模型做约束解码用的枚举。有了它模型就编不出新角色。
    std::vector<std::string> character_ids() const;
    std::vector<std::string> location_ids() const;

    /// 检查分镜表引用的 id 是否都已注册。返回问题列表。
    std::vector<std::string> validate_references(
        const std::vector<std::string>& char_ids,
        const std::vector<std::string>& loc_ids) const;

    std::vector<std::string> validate() const;
};

/// 从身份描述里猜性别。猜不出来返回空串。
/// 身份描述里通常会写「一位年轻的女性」这类话，够用了。
std::string guess_gender(const std::string& identity);

/// 从服务端给的音色列表里挑一条参考音频。
///
/// 先按性别分，再在同性别里优先中文样本。顺序不能反过来：
/// 唯一那条中文样本是男声，反过来的话女角色会被配上男声，
/// 这比带一点口音难听得多。
std::optional<std::string> pick_voice(const std::vector<std::string>& available,
                                      const std::string& gender,
                                      int index = 0);

}  // namespace changji::models
