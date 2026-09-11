// 参考图的提示词片段。**这个文件就是出处，直接改它。**
// 新写的，没有 Python 对应物，不受逐字节约束。
//
// 这些片段会和角色/场景的外观块拼在一起，**外观块那一半是逐字节固定的**
// （Character::render_prompt），这里只在它前后加朝向、背景和质量要求。

#pragma once

namespace changji::stages::prompt {

// ---- 角色三视图 ----
//
// 背景写死成纯色：**参考图是给角色用的，不是给场景用的**。背景里有内容的
// 话，后面每一镜都会把那点内容当成这个人身上带的，一路复制下去。
inline constexpr const char* kRefCharacterHead =
    R"CJ(角色设定参考图，单人全身像，纯灰色背景，均匀柔光)CJ";

inline constexpr const char* kRefCharacterTail =
    R"CJ(站姿自然，表情平静，画面清晰，细节完整)CJ";

inline constexpr const char* kPoseFront =
    R"CJ(正面朝向镜头)CJ";

inline constexpr const char* kPoseThreeQuarter =
    R"CJ(四分之三侧身，脸朝向镜头左侧)CJ";

inline constexpr const char* kPoseBack =
    R"CJ(背对镜头，看得清发型和背影轮廓)CJ";

// ---- 场景空景图 ----
inline constexpr const char* kRefLocationHead =
    R"CJ(场景参考图，空镜，广角，画面清晰)CJ";

/// **这一条是硬要求，所以单独一段。**
///
/// 空景图是场景一致性的锚点，后面每一镜都拿它当底子。里面站个人的话，
/// 那个人会被当成这个地方的一部分，一路复制到每一镜里去——而那是个
/// 不属于任何角色、也没法改的人。
inline constexpr const char* kRefLocationNoPeople =
    R"CJ(画面里没有任何人物)CJ";

// ---- 负向 ----
//
// 比出片那份多压两样：
// **多人**——三视图里多出一个人，后面分镜就分不清哪个是主角；
// **文字水印**——参考图上的字会被当成服装花纹或者墙上的画复制下去。
inline constexpr const char* kRefNegativeExtra =
    R"CJ(，多个人物，人群，文字，水印，签名，拼图，分屏，裁切)CJ";

}  // namespace changji::stages::prompt
