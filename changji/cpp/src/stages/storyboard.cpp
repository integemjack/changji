#include "stages/storyboard.hpp"

#include "stages/limits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include <cstring>

// 落位时要估台词念多久，免得一镜塞到串音
#include "stages/audio_plan.hpp"
#include "stages/json_extract.hpp"
// 段头识别（count_beats 要跳过「【开场钩子 0–5 秒】」那一行）
#include "stages/script.hpp"
#include "stages/shot_schema.inc.hpp"
#include "stages/storyboard_prompt.inc.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 对应 Python 的 f"{x:g}"。
///
/// C 的 %g 和 Python 的 :g 是同一套规则（默认 6 位有效数字，
/// 指数超出 [-4, 6) 才转科学计数法），所以直接用。
/// 提示词里的"总时长 300 秒"就靠它——写成 300.0 秒就破契约了。
std::string format_g(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return std::string(buf);
}

/// Python 的 round()：**银行家舍入**，不是四舍五入。
///
/// round(0.5) 是 0 不是 1，round(2.5) 是 2 不是 3。配额分配里用了三次，
/// 用错的话镜头数会差一个——差一个就意味着提示词里的配额和实际不符，
/// 模型会照着提示词产出，然后校验不过。
///
/// std::nearbyint 在默认舍入模式（FE_TONEAREST）下就是这个语义。
long py_round(double v) { return static_cast<long>(std::nearbyint(v)); }

/// 从 pydantic 导出的 Shot schema，解析一次缓存起来。
const ordered& shot_schema_base() {
    static const ordered base =
        ordered::parse(prompt::kShotSchemaJson);
    return base;
}

/// 分镜阶段允许大模型填的字段。
const std::set<std::string>& llm_shot_fields() {
    static const std::set<std::string> kFields = [] {
        std::set<std::string> s;
        for (const char* f : prompt::kLlmShotFields) s.insert(f);
        return s;
    }();
    return kFields;
}

/// 取 JSON 里的字符串，缺了或不是字符串就给默认值。
std::string str_or(const json& obj, const char* key, const std::string& def = {}) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

/// 对应 Python 的真值判断：缺失、null、0、空串、空数组都算假。
bool truthy(const json& obj, const char* key) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) return !it->get<std::string>().empty();
    if (it->is_array() || it->is_object()) return !it->empty();
    return true;
}

std::string join_lines(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\n";
        out += parts[i];
    }
    return out;
}

}  // namespace

// ---- 视频模型的限制 ----

int VideoLimits::frames_for(double duration_s, int fps) const {
    const int step = std::max(1, frame_step);
    const int base = std::max(0, frame_base);
    // **先夹再取整。** MSVC 的 long 是 32 位，给个很大的秒数（比如拿
    // frames_for(1e9) 问"最长能多少帧"）会让 duration_s * fps 溢出，
    // 溢出之后帧数变成负的，档位表退化成只剩一档 2 秒——一连串分镜用例
    // 跟着挂，而根因离现场很远。
    const double capped =
        std::clamp(duration_s * (fps <= 0 ? 24 : fps), 0.0, 1.0e7);
    const long raw = py_round(capped);
    // 向上对齐到 step*k + base。给 sd.cpp 一个不在格子上的数它会自己
    // 往上对齐，而对齐到哪儿不告诉你——那正是"成片比分镜表长一点点"的来源。
    // 至少一个完整的格子：一帧的视频没有意义，而分镜表里出现零点几秒的
    // 镜头会一路走到装配。老实现那句 max(1L, n) 就是干这个的。
    long k = 1;
    if (raw > base) {
        k = std::max(1L, (raw - base + step - 1) / step);
    }
    long frames = static_cast<long>(step) * k + base;
    // **夹到上限时也要落在格子上。** 上限本身常常不在格子上——MiniMax-H3
    // 说能出 15 秒，15×24 = 360，而 (360-5)/17 = 20.88 不是整数。直接
    // min(frames, 360) 会交出一个非法帧数，sd.cpp 再把它向上对齐到 362，
    // 反而**超过**了上限。所以往下取到不超过上限的那个合法值（345）。
    const long cap = std::max(1, max_frames);
    if (frames > cap) {
        const long kk = cap > base ? (cap - base) / step : 0;
        frames = static_cast<long>(step) * kk + base;
    }
    if (frames < base) frames = base;
    if (frames < 1) frames = 1;
    return static_cast<int>(frames);
}

int VideoLimits::max_frames_on_grid() const {
    const int step = std::max(1, frame_step);
    const int base = std::max(0, frame_base);
    const long cap = std::max(1, max_frames);
    const long k = cap > base ? (cap - base) / step : 0;
    long frames = static_cast<long>(step) * k + base;
    if (frames < 1) frames = 1;
    return static_cast<int>(frames);
}

double VideoLimits::max_duration_s(int fps) const {
    const int f = fps <= 0 ? 24 : fps;
    // 按**真正生成得出来**的最长帧数算，不是按配置里那个数：上限不在格子上
    // 的时候两者差一截（360 → 345），档位表照 360 排就会排出根本出不来的档。
    return static_cast<double>(max_frames_on_grid()) / static_cast<double>(f);
}

double VideoLimits::real_duration_s(double duration_s, int fps) const {
    const int f = fps <= 0 ? 24 : fps;
    return static_cast<double>(frames_for(duration_s, f)) /
           static_cast<double>(f);
}

std::vector<double> VideoLimits::duration_slots(int fps) const {
    // 候选一路排到 15 秒：换上能出长镜头的模型时（MiniMax-H3 能到 15 秒），
    // 档位表跟着放开，一集就不必被切成十几个五秒片段。上限小的时候后面
    // 那几档自然被滤掉，老项目一点不变。
    const double limit = max_duration_s(fps);
    const double all[] = {2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0, 15.0};
    std::vector<double> out;
    for (const double s : all) {
        if (s <= limit) out.push_back(s);
    }
    if (out.empty()) out.push_back(2.0);
    return out;
}

VideoLimits guess_video_limits(const std::string& video_model_file,
                               bool llm_encoder) {
    std::string low;
    for (char c : video_model_file) {
        const unsigned char u = static_cast<unsigned char>(c);
        low += (u >= 'A' && u <= 'Z') ? static_cast<char>(u - 'A' + 'a') : c;
    }
    const auto has = [&low](const char* w) {
        return low.find(w) != std::string::npos;
    };

    VideoLimits wan;          // 最保守的那一档
    wan.max_frames = 121;
    wan.frame_step = 4;
    wan.frame_base = 1;

    VideoLimits h3;           // 15 秒 × 24fps，帧数 17k+5
    h3.max_frames = 360;
    h3.frame_step = 17;
    h3.frame_base = 5;
    h3.max_pixels = 1032192;  // 1344 × 768，官方 canvas_max_pixels

    if (has("minimax") || has("hailuo") || has("h3")) return h3;
    if (has("wan")) return wan;
    // 名字认不出就看编码器：H3 那一路挂的是 video_llm，Wan 挂的是 t5xxl。
    return llm_encoder ? h3 : wan;
}

VideoLimits cap_by_vram(VideoLimits limits, double vram_gb, double resident_gb,
                        int fps) {
    // 探测不到显卡就别自作主张放开。宁可短。
    if (vram_gb <= 0.0) return limits;

    // 锚点：5090 上 1280×704、H3 权重全放内存，一个五秒镜头（124 帧）的
    // 计算缓冲实测 ~14.6 GB。按每帧线性摊——**这是外推，不是实测曲线**，
    // 所以再留三成余量。真要放开长镜头，先拿那张卡跑一组 5/8/12 秒量峰值。
    constexpr double kBufferPerFrame = 14.6 / 124.0;   // ≈ 0.118 GB/帧
    // 留一半。**这个系数是保守拍的，不是量出来的**：定成 0.5 时 5090
    // （32.6 GB）算出来 138 帧 ≈ 5.75 秒，档位还是 {2,3,4,5}，和实测跑得动
    // 的那一档一致；大卡才会放开（48 GB → 8 秒档，80 GB → 12 秒档）。
    // 拿到 5/8/12 秒的峰值显存实测之后，这里应该换成真曲线。
    constexpr double kHeadroom = 0.5;

    // **再小也别低于最保守那一档（五秒）。** 跑不了五秒镜头的卡，整条流水线
    // 本来也跑不动；把上限夹到一两秒只会让分镜排出一堆没法用的碎片，而且
    // 单元测试跑在什么卡上就成了测试结果的一部分——那种失败离根因极远
    // （报的是「时长吸附不对」，根子在这儿）。
    // **按秒定，不按帧定。** 写死 121 帧的话，换成 17k+5 那个格子会向下取到
    // 107 帧 = 4.458 秒，反而不够五秒——同一个下限在不同格子上要落在
    // 各自合法的那个数上。
    constexpr double kNeverBelowSeconds = 5.0;
    const int floor_frames =
        std::min(limits.max_frames, limits.frames_for(kNeverBelowSeconds, fps));

    const double usable = (vram_gb - std::max(0.0, resident_gb)) * kHeadroom;
    if (usable <= 0.0) {
        // 权重就把卡占满了：不放开，退回保守那一档。
        limits.max_frames = floor_frames;
        return limits;
    }
    const int fits = static_cast<int>(usable / kBufferPerFrame);
    limits.max_frames = std::max(floor_frames, std::min(limits.max_frames, fits));
    return limits;
}

namespace {

VideoLimits& mutable_video_limits() {
    static VideoLimits v;
    return v;
}

}  // namespace

const VideoLimits& video_limits() { return mutable_video_limits(); }

void set_video_limits(VideoLimits v) { mutable_video_limits() = std::move(v); }

double max_shot_duration_s(int fps) {
    return video_limits().max_duration_s(fps);
}

const std::vector<double>& duration_slots() {
    // **不能再 static 缓存了。** 原来这里缓存着，注释说是「照抄 Python 的
    // 模块级常量，改了 fps 也不重算」——而 Python 引擎 2026-09-10 就删了，
    // 那个理由不成立；现在上限来自配置，缓存住就等于配置改了不生效。
    //
    // 按当前 limits 算一份留着，limits 变了才重算：snap_duration 会在
    // rebalance 的循环里调很多次，每次构造一个 vector 没必要。
    static thread_local std::vector<double> cached;
    static thread_local int cached_frames = -1;
    static thread_local int cached_step = -1;
    static thread_local int cached_base = -1;
    const VideoLimits& v = video_limits();
    if (cached.empty() || cached_frames != v.max_frames ||
        cached_step != v.frame_step || cached_base != v.frame_base) {
        cached = v.duration_slots();
        cached_frames = v.max_frames;
        cached_step = v.frame_step;
        cached_base = v.frame_base;
    }
    return cached;
}

double DurationQuota::total_s() const {
    // 求和顺序要和 Python 一致。那边 slots 是插入序，而 for_duration
    // 的插入序正好是档位升序，和 std::map 的遍历序相同。
    double sum = 0.0;
    for (const auto& [d, n] : slots) sum += d * n;
    return sum;
}

int DurationQuota::shot_count() const {
    int sum = 0;
    for (const auto& [d, n] : slots) {
        (void)d;
        sum += n;
    }
    return sum;
}

std::string DurationQuota::describe() const {
    std::vector<std::string> parts;
    for (const auto& [d, n] : slots) {          // map 已经是升序
        if (!n) continue;                        // 对应 Python 的 if n
        parts.push_back(std::to_string(n) + " 个 " + format_g(d) + " 秒镜头");
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "，";
        out += parts[i];
    }
    return out;
}

DurationQuota DurationQuota::for_duration(double target_s) {
    if (target_s <= 0) throw StoryboardError("目标时长必须大于 0");

    // 分配比例。只用真正可生成的档位，长镜头多分一点时长，
    // 短镜头多分一点数量，这样节奏有变化而不是平铺。
    const std::vector<std::pair<double, double>> weights = {
        {2.0, 0.10}, {3.0, 0.22}, {4.0, 0.18}, {5.0, 0.50}};
    const std::vector<double>& all = duration_slots();

    std::vector<std::pair<double, double>> plan;
    for (const auto& [d, w] : weights) {
        if (std::find(all.begin(), all.end(), d) != all.end()) {
            plan.emplace_back(d, w);
        }
    }
    if (plan.empty()) plan.emplace_back(all.back(), 1.0);

    // 按插入序累加。0.10+0.22+0.18+0.50 在浮点下**不等于** 1.0
    // （是 1.0000000000000002），换个顺序加结果又不一样。
    // scale 会因此差一个 ulp，而后面要过 round()——正好卡在半整数上时
    // 那一个 ulp 就决定进位方向，镜头数差一个。
    double weight_sum = 0.0;
    for (const auto& [d, w] : plan) {
        (void)d;
        weight_sum += w;
    }
    const double scale = 1.0 / weight_sum;

    std::map<double, int> slots;
    for (const auto& [dur, share] : plan) {
        slots[dur] = static_cast<int>(
            std::max<long>(0, py_round(target_s * share * scale / dur)));
    }

    // 用最长的档位补足或削减差额
    const double pad = all.back();
    slots[pad] = std::max(1, slots.count(pad) ? slots[pad] : 0);
    double sum = 0.0;
    for (const auto& [d, n] : slots) sum += d * n;
    const double diff = target_s - sum;
    slots[pad] = std::max<int>(1, slots[pad] + static_cast<int>(py_round(diff / pad)));

    DurationQuota q;
    for (const auto& [d, n] : slots) {
        if (n > 0) q.slots[d] = n;
    }
    return q;
}

double snap_duration(double seconds) {
    // min(..., key=...) 取**第一个**最小的，平局时靠前的赢。
    // 用严格小于就是这个语义。
    const std::vector<double>& slots = duration_slots();
    double best = slots.front();
    double best_gap = std::fabs(best - seconds);
    for (std::size_t i = 1; i < slots.size(); ++i) {
        const double gap = std::fabs(slots[i] - seconds);
        if (gap < best_gap) {
            best = slots[i];
            best_gap = gap;
        }
    }
    return best;
}

double ceil_duration(double seconds) {
    for (const double slot : duration_slots()) {
        if (slot >= seconds - 1e-6) return slot;
    }
    return duration_slots().back();
}

int count_beats(const std::string& script) {
    int n = 0;
    std::size_t start = 0;
    while (start <= script.size()) {
        std::size_t end = script.find('\n', start);
        if (end == std::string::npos) end = script.size();
        const std::string line = text::strip_ws(script.substr(start, end - start));
        if (!line.empty() && !is_act_header(line)) ++n;
        if (end == script.size()) break;
        start = end + 1;
    }
    return n;
}

ShotCountBounds shot_count_bounds(const DurationQuota& quota, double target_s,
                                  int beats) {
    const double longest = duration_slots().back();
    const int physical = std::max(
        1, static_cast<int>(std::ceil(target_s / longest - 1e-9)));
    ShotCountBounds b;
    b.min_items = beats > 0 ? std::max(1, std::min(physical, beats)) : physical;
    b.max_items = std::max(b.min_items, 2 * std::max(quota.shot_count(), beats));
    return b;
}

ordered llm_shot_schema(const AssetLibrary& assets, ShotCountBounds bounds) {
    ordered full = shot_schema_base();
    ordered props = full.contains("properties") ? full["properties"]
                                                : ordered::object();
    ordered defs = full.contains("$defs") ? full["$defs"] : ordered::object();

    ordered kept = ordered::object();
    for (const auto& item : props.items()) {
        if (llm_shot_fields().count(item.key())) kept[item.key()] = item.value();
    }

    const std::vector<std::string> char_ids = assets.character_ids();
    const std::vector<std::string> loc_ids = assets.location_ids();
    if (char_ids.empty()) {
        throw StoryboardError("资产库里一个角色都没有。请先生成角色圣经");
    }

    // 角色 id 收紧成枚举。这是防止模型凭空造角色最硬的手段。
    if (defs.contains("CharacterInShot")) {
        ordered& cis = defs["CharacterInShot"];
        if (!cis.contains("properties")) cis["properties"] = ordered::object();
        cis["properties"]["char_id"] = {
            {"type", "string"}, {"enum", char_ids},
            {"description", "必须是已注册角色之一"}};
    }
    if (defs.contains("DialogueLine")) {
        ordered& dl = defs["DialogueLine"];
        if (!dl.contains("properties")) dl["properties"] = ordered::object();
        dl["properties"]["char_id"] = {
            {"anyOf", ordered::array({ordered{{"type", "string"}, {"enum", char_ids}},
                                      ordered{{"type", "null"}}})},
            {"description", "说话角色。旁白留空"}};
        // 时长由配音阶段回填，不让模型猜
        for (const char* gone : {"audio_path", "actual_duration_s", "voice_id"}) {
            dl["properties"].erase(gone);
        }
    }

    if (!loc_ids.empty()) {
        kept["location_id"] = {
            {"anyOf", ordered::array({ordered{{"type", "string"}, {"enum", loc_ids}},
                                      ordered{{"type", "null"}}})}};
    }
    kept["duration_s"] = {{"type", "number"},
                          {"enum", duration_slots()},
                          {"description", "只能取这些值"}};

    // characters 和 dialogue 必须是必填并且带说明。
    // 只给一个 $ref 而不说要填什么，模型会整个略过这两个字段，
    // 结果是分镜里一句台词都没有，配音和口型全部落空。
    kept["characters"] = {
        {"type", "array"},
        {"items", ordered{{"$ref", "#/$defs/CharacterInShot"}}},
        {"description", "本镜出现的角色。没有人物出镜就填空数组"}};
    kept["dialogue"] = {
        {"type", "array"},
        {"items", ordered{{"$ref", "#/$defs/DialogueLine"}}},
        {"description",
         "本镜的台词和旁白，逐句填。剧本里的每一句话都必须落到某个镜头上，"
         "不能丢。这一镜没有人说话就填空数组"}};

    ordered shots_item = ordered::object();
    shots_item["type"] = "object";
    shots_item["properties"] = kept;
    shots_item["required"] = {"shot_id", "scene_id", "order", "first_frame_prompt",
                              "shot_size", "duration_s", "characters", "dialogue"};
    shots_item["additionalProperties"] = false;

    ordered shots = ordered::object();
    shots["type"] = "array";
    shots["items"] = shots_item;
    if (bounds.min_items > 0) shots["minItems"] = bounds.min_items;
    if (bounds.max_items > 0) shots["maxItems"] = bounds.max_items;

    ordered out_props = ordered::object();
    out_props["shots"] = shots;

    ordered out = ordered::object();
    out["type"] = "object";
    out["properties"] = out_props;
    out["required"] = {"shots"};
    out["additionalProperties"] = false;
    out["$defs"] = defs;
    return out;
}

std::string build_storyboard_prompt(const std::string& script,
                                    const AssetLibrary& assets,
                                    const DurationQuota& quota,
                                    const std::string& episode_id) {
    std::vector<std::string> roster_lines;
    for (const std::string& cid : assets.character_ids()) {
        roster_lines.push_back("  " + cid + "：" + assets.characters.at(cid).name);
    }
    std::vector<std::string> place_lines;
    for (const std::string& lid : assets.location_ids()) {
        place_lines.push_back("  " + lid + "：" + assets.locations.at(lid).name);
    }
    const std::string roster = join_lines(roster_lines);
    // 对应 Python 的 "..." or "  （未定义场景，location_id 留空）"
    const std::string places = place_lines.empty()
                                   ? "  （未定义场景，location_id 留空）"
                                   : join_lines(place_lines);

    std::string out;
    out += prompt::kSbSeg0;
    out += roster;
    out += prompt::kSbSeg1;
    out += places;
    out += prompt::kSbSeg2;
    out += quota.describe();
    out += prompt::kSbSeg3;
    out += std::to_string(quota.shot_count());
    out += prompt::kSbSeg4;
    out += format_g(quota.total_s());
    out += prompt::kSbSeg5;
    out += episode_id;
    out += prompt::kSbSeg6;
    out += script;
    out += prompt::kSbSeg7;
    return out;
}

bool link_location(json& item, const std::set<std::string>& known) {
    if (truthy(item, "location_id")) return false;
    const std::string scene = str_or(item, "scene_id");
    if (known.count(scene)) {
        item["location_id"] = scene;
        return true;
    }
    return false;
}

bool is_placeholder_line(const std::string& text) {
    // 先削掉外面套的括号引号，再削掉结尾的标点：模型写的是
    // 「（无台词）」「无台词。」「(N/A)」，核都是同一个词。
    std::string core = strip_wrapper(text::strip_ws(text));
    core = text::strip_ws(text::rstrip_punct(core));
    core = strip_wrapper(core);
    std::string low;
    for (char c : core) {
        const unsigned char u = static_cast<unsigned char>(c);
        low += (u >= 'A' && u <= 'Z') ? static_cast<char>(u - 'A' + 'a') : c;
    }
    for (const char* w : prompt::kNoLine) {
        if (low == w) return true;
    }
    return false;
}

namespace {

/// 把占位台词从这一镜里删掉。删空了就是一个没人说话的镜头，本来就该这样。
void drop_placeholder_dialogue(json& item) {
    const auto it = item.find("dialogue");
    if (it == item.end() || !it->is_array()) return;
    json kept = json::array();
    for (const auto& line : *it) {
        if (line.is_object() && is_placeholder_line(str_or(line, "text"))) continue;
        kept.push_back(line);
    }
    *it = std::move(kept);
}

/// 台词里裹着的旁白剥掉。
///
/// **剧本那边已经剥过一遍了，这儿还要再剥一遍**：分镜的台词有两个来源，
/// 一个是引擎照剧本放进去的（那份干净），另一个是模型自己写的——
/// 「AI 出分镜」这条路上模型会直接写台词，它照样会把原文整句抄进来。
/// 两个入口都堵上，见 stages::strip_speech_tags。
void clean_dialogue_text(json& item) {
    const auto it = item.find("dialogue");
    if (it == item.end() || !it->is_array()) return;
    for (auto& line : *it) {
        if (!line.is_object()) continue;
        const auto tit = line.find("text");
        if (tit == line.end() || !tit->is_string()) continue;
        *tit = strip_speech_tags(tit->get<std::string>(),
                                 str_or(line, "char_id"));
    }
}

}  // namespace

void add_missing_speakers(json& item, const std::set<std::string>& known) {
    if (!item.is_object()) return;
    const auto dit = item.find("dialogue");
    if (dit == item.end() || !dit->is_array() || dit->empty()) return;

    if (!item.contains("characters") || !item["characters"].is_array()) {
        item["characters"] = json::array();
    }
    json& chars = item["characters"];

    std::set<std::string> present;
    for (const auto& c : chars) {
        if (c.is_object() && c.contains("char_id") && c["char_id"].is_string()) {
            present.insert(c["char_id"].get<std::string>());
        }
    }
    for (const auto& line : *dit) {
        if (!line.is_object()) continue;
        const auto sit = line.find("char_id");
        if (sit == line.end() || !sit->is_string()) continue;
        const std::string speaker = sit->get<std::string>();
        if (speaker.empty()) continue;
        if (known.count(speaker) && !present.count(speaker)) {
            chars.push_back(json{{"char_id", speaker}});
            present.insert(speaker);
        }
    }
}

std::vector<Shot> parse_storyboard(const std::string& raw,
                                   const AssetLibrary& assets) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryboardError(e.what());
    }

    json items;
    if (data.is_object()) {
        items = data.contains("shots") ? data["shots"] : json();
    } else {
        items = data;
    }
    if (!items.is_array()) {
        throw StoryboardError("大模型没有返回镜头列表");
    }
    if (items.empty()) throw StoryboardError("大模型返回了空的分镜表");

    std::set<std::string> known_chars;
    for (const auto& kv : assets.characters) known_chars.insert(kv.first);
    std::set<std::string> known_locs;
    for (const auto& kv : assets.locations) known_locs.insert(kv.first);

    std::vector<Shot> shots;
    std::vector<std::string> problems;
    for (std::size_t i = 0; i < items.size(); ++i) {
        json item = items[i];
        if (!item.is_object()) {
            problems.push_back("第 " + std::to_string(i + 1) + " 个镜头不是对象");
            continue;
        }

        if (!item.contains("order")) item["order"] = static_cast<int>(i);

        // float(x or 默认值)：0、null、缺失、空串都落到默认值
        double dur = duration_slots().back();
        if (truthy(item, "duration_s") && item["duration_s"].is_number()) {
            dur = item["duration_s"].get<double>();
        }
        item["duration_s"] = snap_duration(dur);

        // 模型常忘了硬切必须零时长，这里兜一下而不是报错退出。
        //
        // **越界的也要兜。** 原来只管「没填或者填了 0」，填了 -0.4 就原样
        // 留着——然后 validate 说「要在 0 到 2 秒之间」，**整张分镜表连同
        // 另外十二个好镜头一起作废**。实跑撞上过一次，84 秒的显卡时间没了。
        // 这一栏本来就不值得为它丢掉一整集：转场时长是个装饰。
        if (str_or(item, "transition_in", "cut") == "cut") {
            item["transition_dur_s"] = 0.0;
        } else {
            const auto it = item.find("transition_dur_s");
            const double v = (it != item.end() && it->is_number())
                                 ? it->get<double>()
                                 : 0.0;
            item["transition_dur_s"] = (v > 0.0 && v <= 2.0) ? v : 0.4;
        }

        // **先删占位台词再补说话人。** 反过来的话，「（无台词）」那一句
        // 会先把一个角色补进 characters，于是这一镜凭空多了个在场的人。
        drop_placeholder_dialogue(item);
        // 剥旁白要在删占位之后：「（无台词）」不带引号，两步互不干扰，
        // 但顺序反过来会让剥出来的空串被当成一句真台词留下。
        clean_dialogue_text(item);
        add_missing_speakers(item, known_chars);
        link_location(item, known_locs);

        try {
            Shot s = item.get<Shot>();
            const auto errs = s.validate();
            if (!errs.empty()) {
                throw std::runtime_error(errs.front());
            }
            shots.push_back(std::move(s));
        } catch (const std::exception& e) {
            problems.push_back("第 " + std::to_string(i + 1) + " 个镜头不合法：" +
                               e.what());
        }
    }

    if (!problems.empty()) {
        std::vector<std::string> first5(
            problems.begin(),
            problems.begin() + static_cast<long>(std::min<std::size_t>(5, problems.size())));
        throw StoryboardError("分镜表有 " + std::to_string(problems.size()) +
                              " 个镜头不合法：\n" + join_lines(first5));
    }

    // 先进 set 去重，再转 vector——validate_references 收的是 vector，
    // 而 Python 那边传的是集合推导式，重复的 id 只报一次。
    std::set<std::string> used_char_set, used_loc_set;
    for (const Shot& s : shots) {
        for (const auto& c : s.characters) used_char_set.insert(c.char_id);
        if (s.location_id.has_value() && !s.location_id->empty()) {
            used_loc_set.insert(*s.location_id);
        }
    }
    const std::vector<std::string> used_chars(used_char_set.begin(),
                                              used_char_set.end());
    const std::vector<std::string> used_locs(used_loc_set.begin(),
                                             used_loc_set.end());
    const auto ref_problems = assets.validate_references(used_chars, used_locs);
    if (!ref_problems.empty()) {
        throw StoryboardError("分镜引用了未注册的资产：\n" + join_lines(ref_problems));
    }
    return shots;
}

void renumber_shots(std::vector<Shot>& shots, const std::string& episode_id) {
    std::vector<Shot*> by_order;
    by_order.reserve(shots.size());
    for (Shot& s : shots) by_order.push_back(&s);
    // stable_sort：order 相同的保持原有先后，和 Episode::sorted_shots 一致。
    std::stable_sort(by_order.begin(), by_order.end(),
                     [](const Shot* a, const Shot* b) { return a->order < b->order; });
    for (std::size_t i = 0; i < by_order.size(); ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "_sh%03d", static_cast<int>(i) + 1);
        by_order[i]->shot_id = episode_id + buf;
        by_order[i]->order = static_cast<int>(i);
    }
}

namespace {

/// 剧本里的台词，一行一句，只留冒号后面那半截。
///
/// 段头（「【开场钩子 0–5 秒】」）跳过。判「这一行是台词」的办法和
/// ScriptReader 一样：冒号前是个短名字。
/// 剧本里的台词，一句一条，带说话人。
std::vector<std::pair<std::string, std::string>> script_dialogue_pairs(
    const std::string& script) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t start = 0;
    while (start <= script.size()) {
        std::size_t end = script.find('\n', start);
        if (end == std::string::npos) end = script.size();
        const std::string line = text::strip_ws(script.substr(start, end - start));
        if (end == script.size()) start = script.size() + 1;
        else start = end + 1;
        if (line.empty() || is_act_header(line)) continue;

        std::size_t at = line.find("：");
        std::size_t sep = 3;
        if (at == std::string::npos) {
            at = line.find(':');
            sep = 1;
        }
        if (at == std::string::npos || at == 0) continue;
        const std::string name = line.substr(0, at);
        if (text::utf8_len(name) > 12) continue;
        const std::string said = text::strip_ws(line.substr(at + sep));
        if (!said.empty()) out.emplace_back(name, said);
    }
    return out;
}

std::vector<std::string> script_dialogue_lines(const std::string& script) {
    std::vector<std::string> out;
    for (const auto& kv : script_dialogue_pairs(script)) out.push_back(kv.second);
    return out;
}

/// 比对用的写法：空白和标点都去掉。
///
/// **标点不能算数。** 模型把「我来了。晚了七年。」拆成两镜、或者把句号换成
/// 逗号，都是同一句台词落地了；按标点较真的话，这一条会在模型只是换了个
/// 停顿的时候把整集打回去。真丢了的那种是**整句话都不在**，那个照样查得出来。
std::string squash(const std::string& s) {
    static const char* kDrop[] = {
        "，", "。", "！", "？", "、", "；", "：", "…", "—", "～",
        "「", "」", "“", "”", "‘", "’", "（", "）", "《", "》",
    };
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t len = text::utf8_char_len(static_cast<unsigned char>(s[i]));
        const std::string ch = s.substr(i, len);
        i += len;
        if (len == 1) {
            const unsigned char u = static_cast<unsigned char>(ch[0]);
            // ASCII 的空白和标点一并丢
            if (u <= ' ' || std::strchr(",.!?;:\"'()-", u) != nullptr) continue;
        } else {
            bool drop = false;
            for (const char* p : kDrop) {
                if (ch == p) {
                    drop = true;
                    break;
                }
            }
            if (drop) continue;
        }
        out += ch;
    }
    return out;
}

}  // namespace

std::vector<std::string> check_coverage(const std::string& script,
                                        const std::vector<Shot>& shots) {
    // 对应 re.search(r"[：:]\s*\S", script)：找一个冒号，后面跳过空白，
    // 再要求至少一个非空白字符。手写而不用 std::regex——那东西在长输入上
    // 会递归到爆栈，而这里的输入是整集剧本。
    const auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '\v' || c == '\f';
    };
    bool script_has_dialogue = false;
    for (std::size_t i = 0; i < script.size() && !script_has_dialogue; ++i) {
        std::size_t after = 0;
        if (script[i] == ':') {
            after = i + 1;
        } else if (script.compare(i, 3, "：") == 0) {   // 全角冒号 U+FF1A
            after = i + 3;
        } else {
            continue;
        }
        while (after < script.size() &&
               is_ws(static_cast<unsigned char>(script[after]))) {
            ++after;
        }
        if (after < script.size()) script_has_dialogue = true;
    }

    std::vector<std::string> problems;
    std::size_t shot_lines = 0;
    for (const Shot& s : shots) shot_lines += s.dialogue.size();
    if (script_has_dialogue && shot_lines == 0) {
        problems.push_back(
            "剧本里有对白，但分镜表里一句台词都没有。"
            "模型多半漏填了 dialogue 字段");
    }
    const bool any_chars = std::any_of(
        shots.begin(), shots.end(),
        [](const Shot& s) { return !s.characters.empty(); });
    if (!any_chars) {
        problems.push_back(
            "所有镜头的 characters 都是空的，没有任何角色出镜。"
            "模型多半漏填了 characters 字段");
    }

    return problems;
}

int place_missing_dialogue(std::vector<Shot>& shots, const std::string& script,
                           const AssetLibrary& assets) {
    if (shots.empty()) return 0;
    const auto want = script_dialogue_pairs(script);
    if (want.empty()) return 0;

    // 名字 → char_id。剧本里写的是名字，镜头里存的是 id。
    std::map<std::string, std::string> id_of;
    for (const auto& kv : assets.characters) id_of[kv.second.name] = kv.first;

    // 这一句已经落在第几镜；-1 表示没落。
    const int n = static_cast<int>(want.size());
    const int m = static_cast<int>(shots.size());
    std::vector<int> at(want.size(), -1);
    for (int i = 0; i < n; ++i) {
        const std::string key = squash(want[static_cast<std::size_t>(i)].second);
        if (key.empty()) continue;
        for (int s = 0; s < m && at[static_cast<std::size_t>(i)] < 0; ++s) {
            for (const auto& d : shots[static_cast<std::size_t>(s)].dialogue) {
                const std::string got = squash(d.text);
                if (got.find(key) != std::string::npos ||
                    (!got.empty() && key.find(got) != std::string::npos)) {
                    at[static_cast<std::size_t>(i)] = s;
                    break;
                }
            }
        }
    }

    // 这一镜的台词加起来要念多久。
    //
    // **一镜装不下几句话。** 单镜时长有硬上限（视频模型的帧数上限，5 秒），
    // 而配音那一步是按「这一镜台词的总时长」向上吸附着锁时长的
    // （audio.cpp 的 lock_duration）——塞四句进去，配出来十几秒，镜头还是
    // 5 秒，**后面的声音就盖到下一镜上**。audio_plan.hpp 开头警告的就是它。
    const auto spoken = [](const Shot& s) {
        double t = 0.0;
        for (const auto& d : s.dialogue) t += estimate_speech_duration(d.text);
        return t;
    };
    const double cap = max_line_seconds();

    int placed = 0;
    for (int i = 0; i < n; ++i) {
        if (at[static_cast<std::size_t>(i)] >= 0) continue;

        // 前后最近的锚点。这一句应该落在它们之间，先后次序才不乱。
        int prev = -1, next = -1;
        for (int k = i - 1; k >= 0; --k) {
            if (at[static_cast<std::size_t>(k)] >= 0) { prev = at[static_cast<std::size_t>(k)]; break; }
        }
        for (int k = i + 1; k < n; ++k) {
            if (at[static_cast<std::size_t>(k)] >= 0) { next = at[static_cast<std::size_t>(k)]; break; }
        }

        const double need =
            estimate_speech_duration(want[static_cast<std::size_t>(i)].second);
        int lo = prev >= 0 ? prev + 1 : 0;
        int hi = next >= 0 ? next - 1 : m - 1;
        if (lo > hi) {
            // 锚点把区间挤没了（比如前一句已经落在最后一镜）。
            lo = hi = std::clamp(prev >= 0 ? prev : next, 0, m - 1);
        }
        lo = std::clamp(lo, 0, m - 1);
        hi = std::clamp(hi, lo, m - 1);

        // 先在该在的区间里找装得下的。
        int target = -1;
        for (int s = lo; s <= hi; ++s) {
            if (spoken(shots[static_cast<std::size_t>(s)]) + need <= cap) {
                target = s;
                break;
            }
        }
        // 区间里都塞满了就往整集找——**次序略有出入，也比串音强**：
        // 台词的先后人一眼看得出来、拖一下就能改，声音叠在一起是听不清的。
        if (target < 0) {
            for (int off = 0; off < m && target < 0; ++off) {
                for (const int s : {lo - off, hi + off}) {
                    if (s < 0 || s >= m) continue;
                    if (spoken(shots[static_cast<std::size_t>(s)]) + need <= cap) {
                        target = s;
                        break;
                    }
                }
            }
        }
        // 整集都装不下（台词比镜头多得多）：挑最空的那一镜，至少别都堆一处。
        if (target < 0) {
            target = lo;
            for (int s = 0; s < m; ++s) {
                if (spoken(shots[static_cast<std::size_t>(s)]) <
                    spoken(shots[static_cast<std::size_t>(target)])) {
                    target = s;
                }
            }
        }

        Shot& shot = shots[static_cast<std::size_t>(target)];
        models::DialogueLine line;
        line.text = want[static_cast<std::size_t>(i)].second;
        const auto it = id_of.find(want[static_cast<std::size_t>(i)].first);
        // 认不出的名字当旁白。这一层不该猜，剧本那边已经把 speaker 收成
        // 枚举了，认不出来多半真是旁白。
        if (it != id_of.end()) line.char_id = it->second;
        shot.dialogue.push_back(std::move(line));

        // 说话的人必然在场
        if (it != id_of.end()) {
            const bool there = std::any_of(
                shot.characters.begin(), shot.characters.end(),
                [&](const models::CharacterInShot& c) {
                    return c.char_id == it->second;
                });
            if (!there) {
                models::CharacterInShot c;
                c.char_id = it->second;
                shot.characters.push_back(std::move(c));
            }
        }
        at[static_cast<std::size_t>(i)] = target;
        ++placed;
    }
    return placed;
}

std::vector<std::string> missing_dialogue_lines(const std::string& script,
                                                const std::vector<Shot>& shots) {
    std::vector<std::string> said;
    for (const Shot& s : shots) {
        for (const auto& line : s.dialogue) said.push_back(squash(line.text));
    }
    // 一句都没写是 check_coverage 管的事（那张表整个废了）。这里再把剧本里
    // 每一句都列一遍，等于把同一个毛病说两遍。
    if (said.empty()) return {};

    std::vector<std::string> missing;
    for (const std::string& want : script_dialogue_lines(script)) {
        const std::string key = squash(want);
        if (key.empty()) continue;
        // 两头都认：模型有时把一句拆成两镜，有时把两句并成一条。
        const bool found = std::any_of(
            said.begin(), said.end(), [&](const std::string& got) {
                return got.find(key) != std::string::npos ||
                       (!got.empty() && key.find(got) != std::string::npos);
            });
        if (!found) missing.push_back(want);
    }
    return missing;
}

double real_total_s(const std::vector<Shot>& shots, int fps) {
    const VideoLimits& limits = video_limits();
    double total = 0.0;
    for (const Shot& s : shots) total += limits.real_duration_s(s.duration_s, fps);
    return total;
}

std::vector<Shot>& rebalance_durations(std::vector<Shot>& shots, double target_s,
                                       double tolerance_s, int fps) {
    const VideoLimits& limits = video_limits();
    // **量的是成片长度，不是分镜表上那串名义值。** 见 real_total_s 的注释：
    // 按名义值精算到 57.00 秒，片子出来是 62.54 秒，压错了对象。
    double current = real_total_s(shots, fps);
    if (std::fabs(current - target_s) <= tolerance_s) return shots;

    std::vector<Shot*> adjustable;
    for (Shot& s : shots) {
        if (s.dialogue.empty() && !s.duration_locked) adjustable.push_back(&s);
    }
    if (adjustable.empty()) return shots;

    const std::vector<double>& slots = duration_slots();
    double diff = target_s - current;
    const int step = diff > 0 ? 1 : -1;
    int guard = 0;
    while (std::fabs(diff) > tolerance_s && guard < 500) {
        ++guard;
        bool moved = false;
        for (Shot* shot : adjustable) {
            // 先吸附再查表。时长可能不在档位表里：老项目升级、用户手改分镜、
            // 或者档位表本身变过。直接查会越界。
            const double current_slot = snap_duration(shot->duration_s);
            const auto it = std::find(slots.begin(), slots.end(), current_slot);
            const long idx = static_cast<long>(std::distance(slots.begin(), it));
            const long new_idx = idx + step;
            if (new_idx < 0 || new_idx >= static_cast<long>(slots.size())) continue;
            const double cand = slots[static_cast<std::size_t>(new_idx)];
            // 换档换掉的也是**成片**那几秒。名义上 4→5 是 +1 秒，在 H3 的
            // 格子上是 4.458→5.167，只有 +0.709；按名义值记账会把 diff 记
            // 少了，循环提前收手，剩下的偏差全留给成片。
            const double delta = limits.real_duration_s(cand, fps) -
                                 limits.real_duration_s(shot->duration_s, fps);
            if (std::fabs(diff - delta) < std::fabs(diff)) {
                shot->duration_s = cand;
                diff -= delta;
                moved = true;
                if (std::fabs(diff) <= tolerance_s) break;
            }
        }
        if (!moved) break;
    }
    return shots;
}

}  // namespace changji::stages
