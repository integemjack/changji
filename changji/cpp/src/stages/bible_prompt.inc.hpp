// 本文件由 cpp/tools/gen_prompts.py 从 Python 的 build_prompt
// 生成，不要手改。提示词要求和 Python 逐字节一致，手抄错一个字
// 不会报错，只会让模型输出悄悄变一点。

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kBiblePrefix =
    R"CJ(你是一位短剧美术指导。读下面的剧本，产出角色设定和场景设定。

画风是)CJ";

inline constexpr const char* kBibleHintRealistic =
    R"CJ(真人写实，描述用自然的中文短句)CJ";

inline constexpr const char* kBibleHintAnime =
    R"CJ(二次元动漫，描述用简洁的标签式短语)CJ";

inline constexpr const char* kBibleMiddle =
    R"CJ(。

要求：

1. 只写剧本里真实出现的角色和场景，不要自己加人加景。
2. face 这一段会在几十个镜头里被逐字复用，是角色能不能保持一致的关键。
   写具体的、可画出来的特征：发型、发色、脸型、眼型。
   不要写"好看""气质佳"这类无法转成画面的词。
3. identity 用一句话说清性别、年龄段、气质。
4. attire 写这个角色的默认服装。剧情中的换装不在这里写。
5. key 用英文小写和下划线，比如 lin_wan、office_night。
6. lighting 要说清时间和光质，比如"夜间冷调顶光，霓虹反光"。
7. global_style 是全剧统一的调子，所有镜头都会带上它。

剧本：

)CJ";

inline constexpr const char* kBibleTail =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

}  // namespace changji::stages::prompt
