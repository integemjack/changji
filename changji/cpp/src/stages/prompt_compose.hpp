#pragma once

// 提示词组装。
//
// **这是角色一致性真正生效的地方。** 同一个角色在几十个镜头里拿到的
// 身份层完全相同，因为它是从资产库读出来的同一个字符串——模型碰不到它。
//
// 分层顺序是固定的，**不能改**：
//
//     身份层（角色外观，逐字节不变）
//     场景层（空间与光线，每场固定）
//     镜头层（景别、机位、动作，每镜可变）
//     风格层（全剧统一）
//
// 顺序不能改是因为提示词里靠前的词权重更高。顺序一变画面重心就跟着变，
// 同一个角色在不同镜头里会显得不是同一个人。
//
// ---
//
// 这一层和出图后端无关：ComfyUI 也好、进程内 sd.cpp 也好，
// 拿到的是同一份提示词。所以它属于**契约**，要和 Python 逐字节一致。

#include <stdexcept>
#include <string>
#include <vector>

#include "models/character.hpp"
#include "models/shot.hpp"

namespace changji::stages {

class RenderError : public std::runtime_error {
public:
    explicit RenderError(const std::string& what) : std::runtime_error(what) {}
};

/// 一个镜头拼好的正负提示词。
struct PromptBundle {
    std::string positive;
    std::string negative;
    /// 角色定妆图和场景空景图的相对路径。图像模型那条路会用，
    /// 视频模型那条路忽略。
    std::vector<std::string> reference_images;
};

/// 把分镜表和资产库拼成提示词。
class PromptComposer {
public:
    explicit PromptComposer(models::AssetLibrary assets);

    PromptBundle compose(const models::Shot& shot) const;

    /// 给视频模型的运动描述。**它只管动作，不重复外观**——
    /// 重复的话运动模型会试图重新"画"一遍角色，
    /// 而首帧已经把长相定死了，两者打架的结果是脸在动的过程中变形。
    std::string motion_prompt(const models::Shot& shot) const;

    /// 这条片子是写实线还是动画线。
    ///
    /// 暴露出来是因为**后端那一层也要用它拼提示词**，而后端拿不到资产库。
    /// 原来两个视频后端都是写死 REALISTIC，动画线的项目在那一步
    /// 会用错分隔符（见 video_positive）。
    models::StyleLine style_line() const { return style_line_; }

private:
    models::AssetLibrary assets_;
    models::StyleLine style_line_;
    std::string sep_;
};

/// 景别、机位、运镜的中文说法。写实线用自然语言，模型对中文景别词有反应。
///
/// 单独暴露是为了能测：表里少一项的话，那个景别拼出来是空的，
/// 而空的景别层意味着模型自己决定构图——同一集里景别会乱跳。
const std::string& shot_size_zh(models::ShotSize v);
const std::string& angle_zh(models::CameraAngle v);
const std::string& move_zh(models::CameraMove v);

}  // namespace changji::stages
