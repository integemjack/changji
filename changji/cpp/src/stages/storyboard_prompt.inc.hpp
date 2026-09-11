// **这个文件现在就是提示词的出处，直接改它。**
//
// 原来这里写的是"由 cpp/tools/gen_prompts.py 生成，不要手改"。那个生成器
// 2026-09-10 随 Python 引擎一起删了（见 6d6982a），禁令却留了下来——
// 照着它去找生成器会找不到，而不敢改这个文件就等于提示词冻死在这儿。
// 分镜提示词，按插值点切成八段。拼的顺序是：
//   seg0 + 角色名单 + seg1 + 场景名单 + seg2 + 配额描述 +
//   seg3 + 镜头数 + seg4 + 总时长 + seg5 + 剧集 id + seg6 + 剧本 + seg7

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kSbSeg0 =
    R"CJ(你是一位短剧分镜师。把下面的剧本拆成分镜表。

可用角色（只能用这些 id）：
)CJ";

inline constexpr const char* kSbSeg1 =
    R"CJ(

可用场景：
)CJ";

inline constexpr const char* kSbSeg2 =
    R"CJ(

镜头配额，必须严格按这个数量和时长分配：
  )CJ";

inline constexpr const char* kSbSeg3 =
    R"CJ(
  合计 )CJ";

inline constexpr const char* kSbSeg4 =
    R"CJ( 个镜头，总时长 )CJ";

inline constexpr const char* kSbSeg5 =
    R"CJ( 秒

硬性要求：

1. shot_id 用 )CJ";

inline constexpr const char* kSbSeg6 =
    R"CJ(_sh001 这样的格式，三位数字，按顺序递增。
2. order 从 0 开始递增。
3. duration_s 只能取配额里出现过的值，且各档位的数量必须与配额完全一致。
4. characters 必须填。凡是这一镜里出现的人，都要在这里列出 char_id，
   并填本镜的表情、动作、面部朝向。画面里没有人才填空数组。
   只填这些，绝对不要描述角色的长相、发型、发色、身材或服装样式，
   那些由系统统一管理，你写了会被丢弃并造成前后不一致。
   换装只能通过 wardrobe_state 填一个状态名。
5. first_frame_prompt 描述这一镜的画面：环境、光线、构图、角色的姿态和位置。
   同样不要描述角色长相，系统会自动拼接。
6. dialogue 必须填。剧本里的每一句台词都要落到某个镜头上，一句都不能丢。
   说话的人填 char_id，旁白留空。这一镜没人说话才填空数组。
   说话的角色也必须同时出现在 characters 里。
7. 同一场景内连续镜头尽量复用 camera_id，避免越轴。
8. transition_in 默认 cut 且 transition_dur_s 必须为 0；
   只有场景切换才用 dissolve，此时 transition_dur_s 填 0.4。
9. continuity_notes 记录需要与前后镜保持一致的细节，比如道具在哪只手。
10. 如果剧本里有信息不足以确定画面的地方，写进 missing_info，不要自己编。

剧本：

)CJ";

inline constexpr const char* kSbSeg7 =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

}  // namespace changji::stages::prompt
