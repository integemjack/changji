#include "stages/prompt_compose.hpp"

#include "stages/storyboard.hpp"  // motion_covering

#include <cstdio>
#include <map>
#include <utility>

#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

const std::string kEmpty;

/// 表里查不到就返回空串。
///
/// 返回空串而不是抛异常，是照抄 Python 的 dict.get(k, "")。
/// 抛异常的话，加一个新枚举值忘了补表会让整条流水线挂掉；
/// 返回空串只是那一层少一个词。前者更响亮但也更容易在半夜炸掉一整晚的活。
template <typename K>
const std::string& lookup(const std::map<K, std::string>& m, K key) {
    const auto it = m.find(key);
    return it == m.end() ? kEmpty : it->second;
}

/// 用分隔符连接，**跳过空串，并且削掉每一段尾巴上的断句标点**。
///
/// 跳空是必须的：不跳的话缺一层就多一个"，，"，
/// 而那个双逗号会被模型当成一个停顿信号。
///
/// **削尾标点是 2026-09-13 补的，治的是同一个病的另一半。** 大模型写完一段
/// 描述习惯性地点一个句号，而这里紧跟着又接一个「，」，拼出来是"。，"——
/// 和双逗号一样是个坏掉的分隔符，而且它一直在那儿：
///
/// | | 以句号结尾的 |
/// |---|---|
/// | walk_c 的 `first_frame_prompt` | 61 / 61 |
/// | 新出的那份 `first_frame_prompt` | 46 / 47 |
/// | 新出的那份 `motion_prompt` | 51 / 52 |
///
/// 也就是说**每一张首帧的提示词里都有一个"。，"**，从来没人看见——因为
/// 它不报错，只是让模型多读到一个停顿。运动描述那一栏以前是空的，
/// 所以这半边直到 `motion_prompt` 变成必填才露出来。
///
/// 削的是 `rstrip_punct` 那一套（。.；;，,、和空格），**不含 ！？ 和引号**：
/// "他说：「走！」"那种结尾是内容，不是断句。段落中间的句号一个不动——
/// 那是真的句子结构，只有贴着分隔符的那个是噪声。
std::string join_nonempty(const std::vector<std::string>& parts,
                          const std::string& sep) {
    std::string out;
    bool first = true;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        const std::string clean = text::rstrip_punct(p);
        if (clean.empty()) continue;
        if (!first) out += sep;
        out += clean;
        first = false;
    }
    return out;
}

/// prompts.toml 里 [compose.*] 那几张词表读成 map。键是枚举名（和 to_string
/// 出来的一样），所以下面三个函数按名字查，查不到还是空串——理由见 lookup。
template <std::size_t N>
std::map<std::string, std::string> zh_table(
    const std::pair<const char*, const char*> (&rows)[N]) {
    std::map<std::string, std::string> m;
    for (const auto& [key, zh] : rows) m.emplace(key, zh);
    return m;
}

}  // namespace

const std::string& shot_size_zh(ShotSize v) {
    static const auto m = zh_table(prompt::compose::kShotSize);
    return lookup(m, std::string(to_string(v)));
}

const std::string& angle_zh(CameraAngle v) {
    static const auto m = zh_table(prompt::compose::kCameraAngle);
    return lookup(m, std::string(to_string(v)));
}

const std::string& move_zh(CameraMove v) {
    static const auto m = zh_table(prompt::compose::kCameraMove);
    return lookup(m, std::string(to_string(v)));
}

const std::string& lens_zh(Lens v) {
    static const auto m = zh_table(prompt::compose::kLens);
    return lookup(m, std::string(to_string(v)));
}

PromptComposer::PromptComposer(AssetLibrary assets)
    : assets_(std::move(assets)), style_line_(assets_.style.style_line) {
    // 动漫线是 Danbooru 标签串，用英文逗号加空格；写实线是自然语言，
    // 用中文逗号。两条线的分隔符不一样，混用会让标签串里冒出中文标点，
    // 而那对 Danbooru 词表训练出来的模型是噪声。
    sep_ = style_line_ == StyleLine::ANIME ? ", " : "，";
}

PromptBundle PromptComposer::compose(const Shot& shot) const {
    return compose_with(shot, shot.first_frame_prompt);
}

PromptBundle PromptComposer::compose_end(const Shot& shot) const {
    if (!shot.last_frame_prompt.has_value() ||
        text::strip_ws(*shot.last_frame_prompt).empty()) {
        return compose(shot);
    }
    return compose_with(shot, *shot.last_frame_prompt);
}

PromptBundle PromptComposer::compose_with(const Shot& shot,
                                          const std::string& picture) const {
    std::vector<std::string> layers;
    std::vector<std::string> refs;

    // ---- 身份层。逐字节从资产库拼出来，模型碰不到。----
    //
    // **大特写（ECU）不带身份层。** 2026-09-13 q4_full sh004 实见：分镜要的
    // 是「豪华卧室床头柜特写。一部智能手机屏幕亮起……只能看到李浩然模糊的
    // 手部阴影」，角色列表里挂着他，身份层（名字、脸、身材、衣着）排在最前，
    // 出图模型就画了一个拿手机站在床边的全身人像；换种子重出还是。出片模型
    // 拿到这张和提示词矛盾的首帧，半路硬切到手机特写。同一镜四种拼法各出
    // 一遍：身份层在前→全身人像；挪到画面描述后面→还是全身人像；只留脸的
    // 描述放后面→一张脸浮在镜子里；**整个去掉→前景手机、背景虚化的人**，
    // 唯一对得上分镜的一张。
    //
    // 大特写本来就只装得下一个局部（手、眼、物件），角色的全身描述在这一
    // 档里没有位置；画面描述会点名是谁的眼、谁的手，够了。代价是脸的大特写
    // 少了那几个字的长相描述——两秒的插入镜头，可以接受。
    //
    // **场景层同样不带。** 去掉身份层之后同一镜再出：前景是手机，背景还是
    // 整间卧室加一个虚化的人——场景层那句「豪华卧室，柔和自然光……」把
    // 整个房间拉了进来，出片模型在 1.7 秒处又切到真正的手机大特写。连场景
    // 层也去掉之后才是分镜要的那张：手、手机、床头柜、床的一角。画面描述
    // 里本来就写着"只能看到床的一角"，大特写要的环境就那么一点，靠它够了。
    const bool insert_shot = shot.shot_size == ShotSize::ECU;
    for (const CharacterInShot& in_shot : shot.characters) {
        const auto it = assets_.characters.find(in_shot.char_id);
        if (it == assets_.characters.end()) {
            throw RenderError("镜头 " + shot.shot_id + " 引用了未注册角色 " +
                              in_shot.char_id);
        }
        if (insert_shot) continue;   // 校验照做，层不拼、参考图不带
        const Character& c = it->second;
        std::vector<std::string> beats = {
            c.render_prompt(style_line_, in_shot.wardrobe_state)};
        if (!in_shot.expression.empty()) beats.push_back(in_shot.expression);
        if (!in_shot.action.empty()) beats.push_back(in_shot.action);
        layers.push_back(join_nonempty(beats, sep_));

        const auto ref = c.ref_for_pose(to_string(in_shot.face_pose));
        if (ref.has_value() && !ref->empty()) refs.push_back(*ref);
    }

    // ---- 场景层 ----
    if (shot.location_id.has_value() && !shot.location_id->empty()) {
        const auto it = assets_.locations.find(*shot.location_id);
        if (it == assets_.locations.end()) {
            throw RenderError("镜头 " + shot.shot_id + " 引用了未注册场景 " +
                              *shot.location_id);
        }
    }
    if (!insert_shot && shot.location_id.has_value() && !shot.location_id->empty()) {
        const auto it = assets_.locations.find(*shot.location_id);
        layers.push_back(it->second.render_prompt(style_line_));
        if (it->second.ref_empty.has_value() && !it->second.ref_empty->empty()) {
            refs.push_back(*it->second.ref_empty);
        }
    }

    // ---- 镜头层 ----
    //
    // 景别、机位、焦段一句；然后是这一镜的光；然后是画面描述。
    // 焦段和光是 2026-09-14 加的（docs/电影质感方案.md）：没填时
    // （老分镜表）一个字不加，语料照旧。光排在场景层之后是有意的——
    // 场景资产那句 lighting 是一场戏的基调，这一句是这一镜的，要盖过它。
    layers.push_back(join_nonempty(
        {shot_size_zh(shot.shot_size), angle_zh(shot.camera_angle),
         lens_zh(shot.lens)},
        sep_));
    if (!text::strip_ws(shot.lighting).empty()) layers.push_back(shot.lighting);
    if (!picture.empty()) {
        layers.push_back(picture);
    }

    // ---- 风格层 ----
    //
    // **这里不给默认值，补默认是读资产库那一步的事**（见
    // ProjectStore::load_assets）。在这儿补的话，同一份 assets.json 走
    // 出图和走界面显示会得到两种画风，而用户在项目页的「画风」框里看到的
    // 是空的——他改不了一个看不见的东西。
    // 这一段还和 Python 逐字节对拍，见本文件开头。
    if (!assets_.style.global_style.empty()) {
        layers.push_back(assets_.style.global_style);
    }

    // 注意最后这一步过滤的是 **strip 之后为空**，不是原串为空。
    // 对应 Python 的 `if p.strip()`。一层里只有空格的话要整层丢掉，
    // 留着就是一个孤零零的分隔符。
    std::vector<std::string> kept;
    for (const auto& l : layers) {
        if (!text::strip_ws(l).empty()) kept.push_back(l);
    }

    PromptBundle out;
    // **ASCII 圆括号和方括号在 sd.cpp 里是权重语法，这里没有转义。**
    //
    // stable-diffusion.cpp 的 parse_prompt_attention 把 ( ) 当加权、
    // [ ] 当减权，\( 才是字面的括号。它自己的注释里有例子：
    //     parse_prompt_attention("(unnecessary)(parens)")
    //       -> [["unnecessaryparens", 1.1]]
    // 括号被吃掉、里面的词被加权。更糟的是不配对的那种：
    //     parse_prompt_attention("(unbalanced") -> [["unbalanced", 1.1]]
    // 一个孤零零的左括号会把它后面的词全部加权。
    //
    // 提示词里混着大模型写的分镜描述，它偶尔会吐 ASCII 括号——那时候
    // 出来的图会悄悄变一点，**不报错也不好察觉**。
    //
    // **为什么先不转义。** 转了的话，有人故意在资产库里写 "(红裙:1.3)"
    // 调权重的能力就没了。这个能力没写进文档、界面上也没有，但它是通的。
    // 两边都是"悄悄地不按预期工作"，而这个项目自己从不使用权重语法
    // （2026-09-11 查过：stages/ 和 models/ 下一处都没有，金语料里的
    // 提示词是纯中文全角标点）。
    //
    // 要动的话判据是这个：**用户想不想要权重这个能力**。
    //   想要 -> 保持现状，顶多在拼完之后检一遍不配对的括号并告警；
    //   不想要 -> 把 ( ) [ ] 全转义成 \( 那种字面形式，一了百了。
    out.positive = join_nonempty(kept, sep_);
    // 负向提示词是镜头自己的加上全剧的。镜头级的在前——
    // 它是针对这一镜的具体问题加的，权重该更高。
    out.negative = join_nonempty(
        {shot.negative_prompt, assets_.style.negative_prompt}, sep_);
    // 视频那份：镜头自己的 + 全剧的视频负向词（prompts.toml [style]）。
    // 图像那份不带过去，理由见 PromptBundle::negative_video。
    out.negative_video = join_nonempty(
        {shot.negative_prompt, prompt::style::kNegativeVideo}, sep_);
    out.reference_images = std::move(refs);
    return out;
}

std::string PromptComposer::motion_prompt(const Shot& shot) const {
    std::vector<std::string> actions;
    for (const CharacterInShot& in_shot : shot.characters) {
        if (!in_shot.action.empty()) actions.push_back(in_shot.action);
    }
    // **用之前再夹一次时长。**
    //
    // 排分镜和改时长那两处已经对齐过（storyboard.cpp 的 motion_covering），
    // 但**盘上的老分镜表不会自愈**：2026-09-16 实测，修复之前排的一集里
    // 一个 6 秒的镜头挂着 `[0-15秒]`、一个 2 秒的挂着 `[0-5秒]`。那些表
    // 不重排就一直是错的，而重排会把人手改过的东西一起冲掉。
    //
    // 夹在**用**的这一刻，老表照样出对的片子，一个字都不用人管。
    // 对已经对齐的表这一步是恒等变换。
    const std::string motion =
        motion_covering(text::strip_ws(shot.motion_prompt), shot.duration_s);

    // 分镜模型已经按时间码分段：运镜词并进第一段（紧跟第一个「]」），
    // 角色动作接在最后。
    if (!motion.empty() && motion.front() == '[') {
        const std::size_t close = motion.find(']');
        if (close != std::string::npos) {
            std::string head = motion.substr(0, close + 1);
            std::string rest = text::strip_ws(motion.substr(close + 1));
            std::vector<std::string> first = {move_zh(shot.camera_move), rest};
            std::string out = head + " " + join_nonempty(first, sep_);
            if (!actions.empty()) out += sep_ + join_nonempty(actions, sep_);
            return out;
        }
    }

    // 没分段：整镜当一段。
    std::vector<std::string> parts = {move_zh(shot.camera_move)};
    if (!motion.empty()) parts.push_back(motion);
    parts.insert(parts.end(), actions.begin(), actions.end());
    const std::string body = join_nonempty(parts, sep_);
    if (body.empty()) return body;
    char dur[32];
    std::snprintf(dur, sizeof(dur), "%g", shot.duration_s);
    return std::string("[0-") + dur + "秒] " + body;
}

std::vector<std::string> shots_without_refs(
    const std::vector<models::Shot>& shots,
    const models::AssetLibrary& assets) {
    // **走 compose 本身，不另写一份判断。** 参考图怎么挑（每个在场角色按
    // face_pose 取一张、场景取空景图、大特写整个不带）全在 compose 里，
    // 照着再写一遍就是第二份，而两份漂开的时候这一条会**放行本该拦住的**。
    const PromptComposer composer(assets);
    std::vector<std::string> out;
    for (const models::Shot& shot : shots) {
        if (shot.shot_size == models::ShotSize::ECU) continue;
        try {
            if (composer.compose(shot).reference_images.empty()) {
                out.push_back(shot.shot_id);
            }
        } catch (const RenderError&) {
            // 引用了没注册的角色/场景。那是另一条错，让出图那一步去说。
        }
    }
    return out;
}

}  // namespace changji::stages
