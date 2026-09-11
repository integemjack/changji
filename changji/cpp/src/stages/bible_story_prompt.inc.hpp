// 从故事出角色圣经的提示词。**这个文件就是出处，直接改它。**
//
// 和隔壁 bible_prompt.inc.hpp 那一份不是一回事，任务变了：
//
//   老的（从剧本出）  读一集剧本，**找出**里面有哪些角色和场景，顺带定妆
//   新的（从故事出）  名单已经在故事里了，这一步只**定妆**
//
// 差别不是措辞问题。老的那条路上，"全剧共用的资产库"其实是从第一集
// 推出来的——后面几集新冒出来的人只能一个个补登记，而补登记的时候
// 模型看不到前面那批人长什么样。名单从故事来之后这个洞才补上。
//
// 这一份是新写的，没有 Python 对应物，**不受逐字节约束**。
//
// 拼接顺序：Prefix + 画风 + Middle + 故事渲染出来的那段 + Tail

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kBibleStoryPrefix =
    R"CJ(你是一位短剧美术指导。下面是一部剧的人物表和场景表，
给里面每一个人、每一个地方定妆。

画风是)CJ";

inline constexpr const char* kBibleStoryMiddle =
    R"CJ(。

要求：

1. **只给下面列出的人和地方定妆，一个不许多，一个不许少。**
   分章那一段是给你看调子用的，不要从里面再挖出新角色新场景——
   挖出来的东西没有 id，后面分镜表指不到它。
2. name 和 location 的 name 必须和下面给的名字**逐字一样**。
   后面每一镜都是按名字找角色，改一个字就找不到了。
3. face 这一段会在几十个镜头里被逐字复用，是角色能不能保持一致的关键。
   写具体的、可画出来的特征：发型、发色、脸型、眼型。
   不要写"好看""气质佳"这类无法转成画面的词。
4. identity 用一句话说清性别、年龄段、气质。
5. attire 写这个角色的默认服装。剧情中的换装不在这里写。
6. key 用英文小写和下划线，比如 lin_wan、office_night。
7. lighting 要说清时间和光质。下面每个地方都给了时间和光，照着它写具体，
   比如"夜间冷调顶光，霓虹反光"。
8. 人物的"想要什么""怎么变"只是让你判断气质用的，
   **不要把它们写进外观**——那是剧情，画不出来。
9. global_style 是全剧统一的调子，所有镜头都会带上它。

)CJ";

inline constexpr const char* kBibleStoryTail =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

// 分章那一段塞进提示词前截断的长度，按**字符**不是字节。
// 人物表和场景表不截：它们是名单，截掉一半就等于漏掉几个人。
inline constexpr std::size_t kBibleChaptersMaxChars = 3000;

}  // namespace changji::stages::prompt
