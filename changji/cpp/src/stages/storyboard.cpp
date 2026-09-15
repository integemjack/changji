#include "stages/storyboard.hpp"

#include "stages/limits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <regex>
#include <string>
#include <vector>

#include <cstring>

// 落位时要估台词念多久，免得一镜塞到串音
#include "stages/audio_plan.hpp"
#include "stages/json_extract.hpp"
// 段头识别（count_beats 要跳过「【开场钩子 0–5 秒】」那一行）
#include "stages/script.hpp"
#include "stages/shot_schema.inc.hpp"
#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

// **不在 prompts.toml 里**：这是认模型输出用的表，不是提示词。原来和提示词
// 放在一个头里，2026-09-14 提示词搬去 prompts.toml 时留在了这儿。

// 台词栏里的占位符。schema 里写着「这一镜没有人说话就填空数组」，模型照样
// 会塞一句「（无台词）」进去——**而那句会被配音念出来**。实跑里十四镜有四镜
// 是这样的。
//
// 和 script.cpp 的 kNoSpeaker 分开两份：那边是说话人栏（填了
// none 就当旁白，**这一句还在**），这边是台词栏（整句都得删掉）。合成一份
// 的话，一句真的由旁白说出来的话会被当成占位符删掉。
inline constexpr const char* kNoLine[] = {
    R"CJ(无台词)CJ",
    R"CJ(无对白)CJ",
    R"CJ(没有台词)CJ",
    R"CJ(没有对白)CJ",
    R"CJ(无人说话)CJ",
    R"CJ(无声)CJ",
    R"CJ(静默)CJ",
    R"CJ(略)CJ",
    R"CJ(无)CJ",
    R"CJ(空)CJ",
    R"CJ(none)CJ",
    R"CJ(n/a)CJ",
    R"CJ(na)CJ",
    R"CJ(null)CJ",
    R"CJ(nil)CJ",
};

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
    h3.native_fps = 24;       // 上游硬改，传别的值它自己覆盖掉

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

VideoLimits cap_by_kernel_limit(VideoLimits limits, int width, int height) {
    if (width <= 0 || height <= 0) return limits;
    // 实测跑得动的最大「像素 × 帧」：704×1280×192（8 秒）。294 帧炸。
    // 来历见头文件。**没有五秒那道地板**：这是硬墙，地板抬上去就是让
    // 分镜排出必炸的镜头——2K 下只剩 46 帧，那就 46 帧。
    constexpr double kMaxPixelFrames = 704.0 * 1280.0 * 192.0;
    const double px = static_cast<double>(width) * static_cast<double>(height);
    const int fits = static_cast<int>(kMaxPixelFrames / px);
    if (fits < limits.max_frames) limits.max_frames = std::max(1, fits);
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

int effective_fps(const VideoLimits& limits, int configured_fps) {
    if (limits.native_fps > 0) return limits.native_fps;
    return configured_fps > 0 ? configured_fps : 24;
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
        if (!line.empty() && !is_act_header(line) && !is_scene_header(line)) ++n;
        if (end == script.size()) break;
        start = end + 1;
    }
    return n;
}

// ---- 按场拆镜 ----

namespace {

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(start, end - start));
        if (end == s.size()) break;
        start = end + 1;
    }
    return out;
}

/// 角色名单和场景名单那两段，整集那条路和按场那条路共用；
/// 整集那条路的输出是逐字节对拍的，这两段就是从它里面抽出来的。
std::string roster_text(const AssetLibrary& assets) {
    std::vector<std::string> lines;
    for (const std::string& cid : assets.character_ids()) {
        lines.push_back("  " + cid + "：" + assets.characters.at(cid).name);
    }
    return join_lines(lines);
}

std::string places_text(const AssetLibrary& assets) {
    std::vector<std::string> lines;
    for (const std::string& lid : assets.location_ids()) {
        lines.push_back("  " + lid + "：" + assets.locations.at(lid).name);
    }
    // 对应 Python 的 "..." or "  （未定义场景，location_id 留空）"
    return lines.empty() ? "  （未定义场景，location_id 留空）" : join_lines(lines);
}

}  // namespace

void parse_scene_body(const std::string& body, SceneBlock& out) {
    out.body = text::strip_ws(body);
    out.time.clear();
    out.inout.clear();
    out.place.clear();
    // 切分隔符：· / ， 、 | ／ ，都当成同一种
    std::vector<std::string> tokens;
    std::string cur;
    std::size_t i = 0;
    const std::string& b = out.body;
    while (i < b.size()) {
        bool hit = false;
        for (const char* sep : {"·", "／", "，", "、", "|", "/"}) {
            const std::string s = sep;
            if (b.compare(i, s.size(), s) == 0) {
                tokens.push_back(text::strip_ws(cur));
                cur.clear();
                i += s.size();
                hit = true;
                break;
            }
        }
        if (!hit) cur += b[i++];
    }
    tokens.push_back(text::strip_ws(cur));

    std::vector<std::string> rest;
    for (const std::string& t : tokens) {
        if (t.empty()) continue;
        if (t == "内" || t == "外" || t == "室内" || t == "室外" || t == "内景" ||
            t == "外景") {
            if (out.inout.empty()) out.inout = t;
            continue;
        }
        bool timey = false;
        if (text::utf8_len(t) <= 4) {
            for (const char* w : {"日", "夜", "晨", "昏", "晚", "午", "黎明", "凌晨",
                                  "白天", "深夜", "傍晚"}) {
                if (t.find(w) != std::string::npos) {
                    timey = true;
                    break;
                }
            }
        }
        if (timey && out.time.empty()) {
            out.time = t;
            continue;
        }
        rest.push_back(t);
    }
    // 剩下的就是地点。多于一截的话拼起来，「咖啡馆 · 靠窗」这种也算一个地点。
    std::string place;
    for (const std::string& r : rest) {
        if (!place.empty()) place += " ";
        place += r;
    }
    out.place = place;
}

std::optional<std::string> resolve_scene_location(const std::string& place_in,
                                                  const AssetLibrary& assets) {
    const std::string place = text::strip_ws(place_in);
    if (place.empty()) return std::nullopt;
    for (const auto& [id, loc] : assets.locations) {
        if (text::strip_ws(loc.name) == place) return id;
    }
    std::optional<std::string> best;
    std::size_t best_len = 0;
    for (const auto& [id, loc] : assets.locations) {
        const std::string name = text::strip_ws(loc.name);
        if (name.empty()) continue;
        const bool hit = name.find(place) != std::string::npos ||
                         place.find(name) != std::string::npos;
        if (hit && name.size() > best_len) {
            best = id;
            best_len = name.size();
        }
    }
    return best;
}

std::vector<SceneBlock> split_scenes(const std::string& script,
                                     const AssetLibrary& assets) {
    std::vector<SceneBlock> scenes;
    std::vector<std::string> pending;   // 第一个场次头之前的行
    SceneBlock* cur = nullptr;
    std::vector<std::vector<std::string>> texts;
    int running = 0;
    for (const std::string& raw : split_lines(script)) {
        int idx = 0;
        std::string body;
        if (parse_scene_header(raw, &idx, &body)) {
            SceneBlock sb;
            sb.index = ++running;   // 序号按出现次序数，模型编的号不作数
            parse_scene_body(body, sb);
            sb.location_id = resolve_scene_location(sb.place, assets);
            scenes.push_back(std::move(sb));
            texts.emplace_back();
            if (scenes.size() == 1) {
                texts.back() = pending;
                pending.clear();
            }
            cur = &scenes.back();
            continue;
        }
        if (cur == nullptr) {
            pending.push_back(raw);
        } else {
            texts.back().push_back(raw);
        }
    }
    if (scenes.empty()) {
        SceneBlock whole;
        whole.index = 0;
        whole.text = script;
        return {whole};
    }
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        // 去掉首尾的空行，中间的留着
        auto& lines = texts[i];
        while (!lines.empty() && text::strip_ws(lines.front()).empty()) {
            lines.erase(lines.begin());
        }
        while (!lines.empty() && text::strip_ws(lines.back()).empty()) {
            lines.pop_back();
        }
        scenes[i].text = join_lines(lines);
    }
    return scenes;
}

void assign_scene_seconds(std::vector<SceneBlock>& scenes, double target_s) {
    if (scenes.empty()) return;
    const double floor_s = duration_slots().front();
    double total_w = 0.0;
    std::vector<double> w(scenes.size());
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        w[i] = static_cast<double>(std::max(1, count_beats(scenes[i].text)));
        total_w += w[i];
    }
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        scenes[i].seconds = std::max(floor_s, target_s * w[i] / total_w);
    }
}

std::string build_scene_storyboard_prompt(const SceneBlock& scene,
                                          int total_scenes,
                                          const AssetLibrary& assets,
                                          const DurationQuota& quota,
                                          const std::string& episode_id,
                                          const std::string& prev_tail) {
    std::string out;
    out += prompt::storyboard_scene::kSeg0;
    out += std::to_string(total_scenes);
    out += prompt::storyboard_scene::kSeg1;
    out += std::to_string(scene.index);
    out += prompt::storyboard_scene::kSeg2;
    out += scene.body.empty() ? std::string("（场次头没写地点）") : scene.body;
    out += prompt::storyboard_scene::kSeg3;
    if (scene.location_id.has_value()) {
        out += prompt::storyboard_scene::kLocKnown;
        out += *scene.location_id;
        out += prompt::storyboard_scene::kLocKnownTail;
    } else {
        out += prompt::storyboard_scene::kLocUnknown;
    }
    out += prompt::storyboard_scene::kSeg4;
    const std::string tail = text::strip_ws(prev_tail);
    if (!tail.empty()) {
        out += prompt::storyboard_scene::kPrevPre;
        out += tail;
        out += prompt::storyboard_scene::kPrevPost;
    }
    out += prompt::storyboard_scene::kSeg5;
    out += roster_text(assets);
    out += prompt::storyboard_scene::kSeg6;
    out += places_text(assets);
    out += prompt::storyboard_scene::kSeg7;
    out += quota.describe();
    out += prompt::storyboard_scene::kSeg8;
    out += std::to_string(quota.shot_count());
    out += prompt::storyboard_scene::kSeg9;
    out += format_g(quota.total_s());
    out += prompt::storyboard_scene::kSeg10;
    out += episode_id;
    out += prompt::storyboard_scene::kSeg11;
    out += scene.text;
    out += prompt::storyboard_scene::kSeg12;
    return out;
}

ordered llm_scene_shot_schema(const AssetLibrary& assets, ShotCountBounds bounds,
                              const std::optional<std::string>& location_id) {
    ordered s = llm_shot_schema(assets, bounds);
    if (!location_id.has_value() || location_id->empty()) return s;
    ordered& item = s["properties"]["shots"]["items"];
    item["properties"]["location_id"] = {
        {"type", "string"},
        {"enum", ordered::array({*location_id})},
        {"description", "这一场的场景，一律填它"}};
    ordered& req = item["required"];
    bool present = false;
    for (const auto& v : req) {
        if (v == "location_id") present = true;
    }
    if (!present) req.push_back("location_id");
    return s;
}

void stamp_scene(std::vector<Shot>& shots, const SceneBlock& scene) {
    for (Shot& s : shots) {
        s.scene_id = "s" + std::to_string(std::max(1, scene.index));
        if (scene.location_id.has_value() && !scene.location_id->empty()) {
            s.location_id = *scene.location_id;
        }
    }
    // 跨场不接帧：上一场的最后一帧是另一个地方。
    if (!shots.empty()) shots.front().continuous_with_prev = false;
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

    // ---- 运动那两栏：必填，而且 camera_move 不留 default ----
    //
    // **2026-09-13 从一份真实项目查出来的。** 雨夜天台 198 镜里
    // `motion_prompt` 空了 198 个、`camera_move` 是 static 的 198 个、
    // `camera_angle` 是 eye_level 的 198 个——三个数都恰好是 `Shot` 结构体的
    // 默认值。而同一份表里 `shot_size` 有五种取值（MS 107、MLS 39、LS 25、
    // CU 18、MCU 9），`characters` 和 `dialogue` 也都填得好好的。
    //
    // 分界线就是 `required`：**在里面的字段模型都填了，不在里面的它整个略过**，
    // 解析时补上结构体默认值，全程一个字不报。措辞救不了这一条——提示词里把
    // 「运动」写十遍，语法上不填仍然合法（chapter_write 那边一路踩出来的同一句：
    // 管得住模型的不是措辞，是它没得选）。
    //
    // 而这两栏恰恰是图生视频**唯一**能照着动的依据：首帧已经把长相、服装、
    // 场景定死了，视频模型收到的运动描述就是
    // `move_zh(camera_move)` + `motion_prompt` + 各角色的 action
    // （见 PromptComposer::motion_prompt）。三项全默认时那段字拼出来是
    // 「固定镜头，站立不动」——**我们花几分钟显卡时间，求它别动**。
    //
    // `minLength` 12：一个带主语和方向的短句大概这么长。**别再往上抬**——
    // 下限高过这一镜真有的内容时，模型会拿 JSON 字段名凑数（写正文那边实测
    // 1.69% 的段落是这么来的）。
    kept["motion_prompt"] = {
        {"type", "string"},
        {"minLength", 12},
        {"maxLength", 400},
        {"description",
         "这几秒画面怎么动：谁在动、朝哪个方向动、快还是慢，镜头跟不跟。"
         "首帧已经定死了长相、服装和场景，这里只写动的部分，不要复述它们"}};
    // 枚举从 $defs 里取，不在这儿抄第二份——`Shot` 里加一种运镜这儿会跟着走。
    // **去掉 default**：留着等于告诉模型「这一栏可以不管」，而它正是这么做的。
    ordered move_enum = ordered::array({"static"});
    if (defs.contains("CameraMove") && defs["CameraMove"].contains("enum")) {
        move_enum = defs["CameraMove"]["enum"];
    }
    kept["camera_move"] = {
        {"type", "string"},
        {"enum", move_enum},
        {"description",
         "这一镜的运镜。只有定格的物件特写、静止的空镜才填 static"}};

    // ---- 机位、焦段、光：同样必填（2026-09-14）----
    //
    // 上面那一轮故意留了 `camera_angle` 当对照组：同一次生成里，进了
    // required 的两栏活了（camera_move 六种取值），没进的那栏 51/51 还是
    // eye_level。机制坐实，而且那轮凑数率是 0，所以这轮把机位和另外两样
    // 「电影质感」的字段一起放进来：焦段（写实模型对字面焦段有反应）和
    // 这一镜的光（行业说光是真实感最强的锚）。
    //
    // ⚠️ 一次进三个必填字段，凑数率要在真实项目上再看一眼：`lighting`
    // 的 minLength 10 是一句「时段 + 光源 + 方向 + 软硬」的下限，别抬。
    ordered angle_enum = ordered::array({"eye_level"});
    if (defs.contains("CameraAngle") && defs["CameraAngle"].contains("enum")) {
        angle_enum = defs["CameraAngle"]["enum"];
    }
    kept["camera_angle"] = {
        {"type", "string"},
        {"enum", angle_enum},
        {"description",
         "这一镜的机位。压迫用 low，脆弱用 high，失衡用 dutch，"
         "交代全局用 overhead；eye_level 只给平静的对话"}};
    // 焦段的枚举去掉 auto：那是「没填」，不该让模型选。
    ordered lens_enum = ordered::array();
    if (defs.contains("Lens") && defs["Lens"].contains("enum")) {
        for (const auto& v : defs["Lens"]["enum"]) {
            if (v != "auto") lens_enum.push_back(v);
        }
    }
    if (lens_enum.empty()) lens_enum = ordered::array({"normal"});
    kept["lens"] = {
        {"type", "string"},
        {"enum", lens_enum},
        {"description",
         "焦段。交代环境和空间用 wide，对话和日常用 normal，"
         "脸的特写和情绪用 portrait，远处的人和压扁的背景用 tele"}};
    kept["lighting"] = {
        {"type", "string"},
        {"minLength", 10},
        {"maxLength", 80},
        {"description",
         "这一镜的光，一句话四样都要有：什么时段、光从哪儿来、"
         "朝哪个方向打、硬还是软"}};
    kept["continuous_with_prev"] = {
        {"type", "boolean"},
        {"description",
         "紧接上一镜的动作（同一场景、同一时刻、动作连着）才填 true"}};
    kept["last_frame_prompt"] = {
        {"anyOf", ordered::array({ordered{{"type", "string"}, {"maxLength", 1200}},
                                  ordered{{"type", "null"}}})},
        {"description",
         "只有这一镜必须落在一个明确的画面上时才填（比如推到某个物件上停住），"
         "否则留空"}};

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
    // `motion_prompt` / `camera_move` 在这儿，理由见上面那一大段：不在这张表里
    // 的字段，模型会整个略过，然后我们拿结构体默认值当成它的选择。
    shots_item["required"] = {"shot_id", "scene_id", "order", "first_frame_prompt",
                              "motion_prompt", "shot_size", "camera_move",
                              "camera_angle", "lens", "lighting",
                              "duration_s", "characters", "dialogue"};
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
    out += prompt::storyboard::kSeg0;
    out += roster;
    out += prompt::storyboard::kSeg1;
    out += places;
    out += prompt::storyboard::kSeg2;
    out += quota.describe();
    out += prompt::storyboard::kSeg3;
    out += std::to_string(quota.shot_count());
    out += prompt::storyboard::kSeg4;
    out += format_g(quota.total_s());
    out += prompt::storyboard::kSeg5;
    out += episode_id;
    out += prompt::storyboard::kSeg6;
    out += script;
    out += prompt::storyboard::kSeg7;
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
    for (const char* w : kNoLine) {
        if (low == w) return true;
    }
    return false;
}

namespace {

/// 把**不是台词**的那些从这一镜的台词里删掉。删空了就是一个没人说话的
/// 镜头，本来就该这样。
///
/// 两类：
///   占位   「（无台词）」「none」——固定词表，见 is_placeholder_line
///   提示   「（脚步声）」「（旁白/环境音）」——整句被圆括号包住的舞台提示，
///          见 stages::is_stage_direction。这类是开放集合，词表补不全，
///          只能从结构上判。
///
/// 不删的话它们会被**念出来**（char_id 为空就走旁白音），字幕上也照写。
void drop_placeholder_dialogue(json& item) {
    const auto it = item.find("dialogue");
    if (it == item.end() || !it->is_array()) return;
    json kept = json::array();
    for (const auto& line : *it) {
        if (!line.is_object()) { kept.push_back(line); continue; }
        const std::string t = str_or(line, "text");
        if (is_placeholder_line(t) || is_stage_direction(t)) continue;
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
        *tit = strip_list_marker(strip_speech_tags(tit->get<std::string>(),
                                                   str_or(line, "char_id")));
    }
}

/// 认不出的枚举取值，当它没填过。
///
/// nlohmann 的枚举反序列化在认不出取值时**静默回落到表里第一项**，不报错。
/// 客户端那条路早就为这件事回头验了一次（`editing.cpp` 的 `parse_enum`，
/// 对拍语料里 `shot_size: "XXL"` 就是一条 400），**模型这条路一直是照单收
/// 的**。
///
/// 最刺眼的是 shot_size：
///
///   缺这个键        → `ShotSize::MS`（中景，结构体默认值）
///   填「medium」    → `ShotSize::ECU`（**大特写**，表里第一项）
///
/// 同一件事——没有可用的取值——两种结果，而且错的那种更离谱：一镜本该是
/// 中景，出来是一张大特写，全程不报错。camera_angle 一样（缺了是 eye_level，
/// 认不出是 low 仰拍）。
///
/// **是抹掉不是报错**：这一栏填错不值得把另外十几个好镜头一起作废——
/// 上面 transition_dur_s 那段是同一条理由（「实跑撞上过一次，84 秒的显卡
/// 时间没了」）。抹掉之后走的是结构体默认值，和模型压根没填这一栏一模一样，
/// 而那条路流水线本来就走得通。
template <typename E>
void drop_if_unknown(json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end()) return;
    // **null 也抹掉。** 这几栏都不是可空的（可空的是 location_id 那种，
    // schema 里写成 anyOf[string, null]）。留着 null 的话，
    // `NLOHMANN_..._WITH_DEFAULT` 展开出来的 `value(key, 默认值)` 会拿这个
    // null 去转枚举——照样落到表里第一项，绕过了这道闸。
    if (!it->is_string()) {
        obj.erase(key);
        return;
    }
    const std::string want = it->get<std::string>();
    if (std::string(to_string(it->get<E>())) != want) obj.erase(key);
}

void drop_unknown_enums(json& item) {
    if (!item.is_object()) return;
    drop_if_unknown<ShotSize>(item, "shot_size");
    drop_if_unknown<CameraAngle>(item, "camera_angle");
    drop_if_unknown<CameraMove>(item, "camera_move");
    drop_if_unknown<Lens>(item, "lens");
    drop_if_unknown<Transition>(item, "transition_in");
    const auto cit = item.find("characters");
    if (cit == item.end() || !cit->is_array()) return;
    for (auto& c : *cit) {
        if (c.is_object()) drop_if_unknown<FacePose>(c, "face_pose");
    }
}

/// 只留下**允许大模型填**的那些字段，台词行里那三项也一并剥掉。
///
/// 上面 `llm_shot_schema` 已经把 schema 裁到 `kLlmShotFields` 了，但那只管
/// "我们要它填什么"——**管不住它实际填了什么**。那份 schema 走的是各家
/// provider 的 response_format，支持得好不好各不相同；不支持的那几家，它
/// 就只是提示词里的一段字。而下面 `item.get<Shot>()` 是照单全收的。
///
/// 漏进来的后果按字段分等级，最重的那几个都不报错：
///
///   · `status` = final_done / locked —— 出片那一步整镜跳过（这两个不在
///     `render_entry_states` 里），表现是"跑完了，这一镜什么都没出"，而侧
///     边栏还会把这一集打上勾；
///   · `frame_path` / `video_path` —— 墙上是破图，装配还会去拼一个根本不
///     存在的文件（`assembly_usable` 只看这个字段非空加状态）；
///   · `duration_locked` = true —— 配音回填的真实时长被挡在外面，画面按一个
///     猜出来的秒数出。
///
/// 台词行里那三项（audio_path / actual_duration_s / voice_id）是同一条规矩
/// 落到输入上：schema 那边已经 erase 过一次（「时长由配音阶段回填，不让模型
/// 猜」），这儿补上它管不到的那一半。voice_id 尤其要剥——它决定这一句用谁
/// 的嗓子，模型编一个出来，配音那边只会在"这个音色服务端没有"时才提一句。
/// 把 `motion_prompt` 里那几段 `[a-b秒]` 的末段夹到整镜时长——短了补满，长了截回。
///
/// **2026-09-16 从三条崩掉的成片查出来的。** 提示词第 6 条写着「段要连起来
/// 盖满整镜的时长」，模型照样短一截：4 秒的镜头只写到 `[0-2秒]`，5 秒的只写
/// 到 `[0-4秒]`。没写到的那一段视频模型自由发挥，而它发挥的方式是**把主体丢
/// 掉**——实测 sh001 前两秒好好的、后两秒屏幕上的「0元」变成「2元」；
/// sh007 前四秒人还在，最后一秒整幅只剩地板和一条椅子腿。崩的位置和缺口
/// 位置一格不差。
///
/// **写长了同样要夹。** sh015 是个 4 秒的镜头，运动写到 `[0-5秒]`：模型按
/// 五秒的节奏演，画面到四秒被截断，动作没走完。两头都错，判据是同一个。
///
/// 措辞救不了这一条（同 camera_move 那一段的结论），而这件事引擎自己算得出来：
/// 末段的结束秒数对不上这一镜的时长，就把末段的上界改成整镜时长。一个字不用问人。
/// 一段都没有的（模型没按格式写）整句包成 `[0-N秒]`：至少时间轴是满的。
void cover_full_duration(json& item) {
    if (!item.contains("motion_prompt") || !item["motion_prompt"].is_string()) return;
    if (!item.contains("duration_s") || !item["duration_s"].is_number()) return;
    const double dur = item["duration_s"].get<double>();
    if (!(dur > 0)) return;
    item["motion_prompt"] =
        motion_covering(item["motion_prompt"].get<std::string>(), dur);
}

}  // namespace

int count_motion_segments(const std::string& motion_prompt) {
    static const std::regex seg(
        R"(\[\s*[0-9]+(?:\.[0-9]+)?\s*-\s*[0-9]+(?:\.[0-9]+)?\s*秒\s*\])");
    return static_cast<int>(std::distance(
        std::sregex_iterator(motion_prompt.begin(), motion_prompt.end(), seg),
        std::sregex_iterator()));
}

std::string defuse_motion(const std::string& in) {
    if (in.empty()) return in;
    // 会把主体带出画、或者要求模型去编第一帧看不见的空间的那些词。
    // 判据和提示词第 6 条那四条一一对应，改一处要改两处。
    static const char* kRisky[] = {
        "走进", "走出", "走向", "走上", "走下", "闯入", "进入画面", "离开画面",
        "出画", "入画", "推开门", "推门", "开门", "门打开", "门被打开",
        "门被带上", "关门", "拉开抽屉", "穿过", "跑出", "跑进", "退出画面",
    };
    // 逐分句扫。`[a-b秒]` 那种时间码不是分句，原样留着——摘掉它会把
    // 时间轴打断，而 motion_covering 还要靠它对齐。
    const std::string kComma = "，";
    const std::string kStop = "。";
    std::vector<std::string> kept;
    std::string cur;
    bool dropped_any = false;
    bool has_text = false;
    const auto flush = [&] {
        if (cur.empty()) return;
        const std::string clause = text::strip_ws(cur);
        cur.clear();
        if (clause.empty()) return;
        for (const char* w : kRisky) {
            if (clause.find(w) != std::string::npos) {
                dropped_any = true;
                // **摘正文，别把开头那个时间码一起摘走。**
                // `[0-5秒] 曾老板走向门口` 整句丢掉的话时间轴就从
                // `[0-5秒]` 变成没有，motion_covering 只好整句重包，
                // 后面那几段的分段也跟着错位。
                if (!clause.empty() && clause.front() == '[') {
                    const auto close = clause.find(']');
                    if (close != std::string::npos) {
                        kept.push_back(clause.substr(0, close + 1));
                    }
                }
                return;
            }
        }
        // 只剩时间码、没有正文的不算正文（下面判"摘完就空了"要用）
        if (clause.find_first_not_of(" \t") != std::string::npos &&
            !(clause.front() == '[' && clause.back() == ']')) {
            has_text = true;
        }
        kept.push_back(clause);
    };
    for (std::size_t i = 0; i < in.size();) {
        if (in.compare(i, kComma.size(), kComma) == 0) {
            flush();
            i += kComma.size();
            continue;
        }
        if (in.compare(i, kStop.size(), kStop) == 0) {
            flush();
            i += kStop.size();
            continue;
        }
        cur += in[i];
        ++i;
    }
    flush();

    // 没摘到东西，或者摘完只剩时间码：原样还回去。
    // **宁可留一个会崩的镜头，也不交一段空的运动描述**——那一段模型
    // 同样会自由发挥，而且连线索都没有了。
    if (!dropped_any || !has_text) return in;

    std::string out;
    for (const std::string& c : kept) {
        if (!out.empty() && out.back() != ']') out += kComma;
        else if (!out.empty()) out += " ";
        out += c;
    }
    return out;
}

std::string motion_covering(const std::string& in, double dur) {
    std::string mp = in;
    if (mp.empty() || !(dur > 0)) return mp;

    // 找最后一个 `[数字-数字秒]`。数字可能带小数点。
    static const std::regex seg(R"(\[\s*([0-9]+(?:\.[0-9]+)?)\s*-\s*([0-9]+(?:\.[0-9]+)?)\s*秒\s*\])");
    std::smatch m;
    std::string tail = mp;
    std::size_t last_at = std::string::npos, last_len = 0;
    double last_end = -1.0;
    std::size_t base = 0;
    while (std::regex_search(tail, m, seg)) {
        last_at = base + static_cast<std::size_t>(m.position(0));
        last_len = static_cast<std::size_t>(m.length(0));
        last_end = std::stod(m[2].str());
        base = last_at + last_len;
        tail = mp.substr(base);
    }
    const std::string want = format_g(dur);
    if (last_at == std::string::npos) {
        // 一段都没有：整句包起来，时间轴至少是满的
        return "[0-" + want + "秒] " + mp;
    }
    if (std::fabs(last_end - dur) < 1e-6) return mp;   // 正好盖满
    const std::string head = mp.substr(last_at, last_len);
    const auto dash = head.find('-');
    if (dash == std::string::npos) return mp;
    const std::string fixed = head.substr(0, dash + 1) + want + "秒]";
    return mp.substr(0, last_at) + fixed + mp.substr(last_at + last_len);
}

namespace {

void keep_llm_fields(json& item) {
    if (!item.is_object()) return;
    for (auto it = item.begin(); it != item.end();) {
        it = llm_shot_fields().count(it.key()) != 0 ? std::next(it)
                                                    : item.erase(it);
    }
    const auto dit = item.find("dialogue");
    if (dit == item.end() || !dit->is_array()) return;
    for (auto& line : *dit) {
        if (!line.is_object()) continue;
        for (const char* gone : {"audio_path", "actual_duration_s", "voice_id"}) {
            line.erase(gone);
        }
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

/// 整批镜头的景别塌成一个值时，按戏本身重排一遍。
///
/// **2026-09-16 实测：17 镜全是 ECU（大特写）。** 同一张表里 camera_move 全是
/// static、camera_angle 全是 low——三个都恰好是各自枚举的**第一个值**。上一轮
/// （camera_move 那一段）治的是「填不填」，这一条治的是「填什么」：进了 required
/// 之后模型确实每栏都填了，但挨个挑 enum[0] 交差。
///
/// 后果不只是单调。大特写起幅本来就没有余地，视频模型再按运动描述一动，主体
/// 直接出画：那一集里第 3 镜五秒钟从「人推门进来」漂成「一只手和一个文件袋悬在
/// 楼梯间」，第 7 镜最后只剩地板和一条椅子腿。**景别塌了，成片就跟着塌。**
///
/// 所以枚举顺序也换了（MS 打头），提示词第 14 条也重写了——但那两样都是
/// 「劝」。这一条是兜底：劝不住的时候，按这一镜真有的内容重排，让它没得选。
///
/// **只在真塌了的时候动手**（八成以上是同一个值）。模型认真分过景别的表
/// 一个字不碰——它比这几行 if 懂戏。
void diversify_shot_sizes(std::vector<Shot>& shots) {
    if (shots.size() < 4) return;   // 三两镜看不出塌没塌

    std::map<ShotSize, int> hist;
    for (const Shot& s : shots) ++hist[s.shot_size];
    int most = 0;
    for (const auto& [sz, n] : hist) most = std::max(most, n);
    const int n = static_cast<int>(shots.size());
    const bool collapsed = most * 10 >= n * 8;
    // **ECU 单独过半也算塌。** 大特写在 PromptComposer 里整个不带身份层和
    // 场景层（那一条是对的，2026-09-13 实测：带了就画成全身人像），也就是
    // **不带参考图**。偶尔一个两秒的插入镜头这么干没问题，半集都这么干就是
    // 整集丢掉角色一致性——而「缺参考图」那道闸门专门跳过 ECU，一声不吭。
    // 2026-09-16 那一集 17 镜全 ECU，没有一张图拿到过定妆参考，远程日志里
    // 十几条「是图像编辑模型，而这一镜一张参考图都没有」。
    const bool too_many_ecu = hist[ShotSize::ECU] * 2 > n;
    if (!collapsed && !too_many_ecu) return;

    const auto emotional = [](const Shot& s) {
        for (const char* w : {"钩", "扣", "反转", "高潮", "揭", "真相", "爆发"}) {
            if (s.beat.find(w) != std::string::npos) return true;
        }
        return false;
    };

    const std::size_t last = shots.size() - 1;
    for (std::size_t i = 0; i < shots.size(); ++i) {
        Shot& s = shots[i];
        const bool has_line = !s.dialogue.empty();
        const int people = static_cast<int>(s.characters.size());

        if (i == 0) {
            // 开场先交代这是哪儿。观众不知道人站在哪儿的时候，
            // 后面所有的特写都是悬空的。
            s.shot_size = ShotSize::LS;
        } else if (people == 0) {
            s.shot_size = ShotSize::MLS;          // 空镜：环境
        } else if (emotional(s)) {
            s.shot_size = ShotSize::CU;           // 情绪那一下才给脸
        } else if (i == last) {
            s.shot_size = ShotSize::MLS;          // 收尾拉开，把情绪放掉
        } else if (has_line && people >= 2) {
            s.shot_size = ShotSize::MS;           // 对手戏要看得见两个人
        } else if (has_line) {
            s.shot_size = ShotSize::MCU;          // 一个人说话
        } else {
            s.shot_size = ShotSize::MS;
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

        // **先洗枚举。** 认不出就当没填，别让它悄悄变成表里第一项
        // （见 drop_unknown_enums）。放在最前面，是因为下面那段
        // transition_dur_s 要读 `transition_in` 来决定硬切是不是该清零——
        // 读到一个认不出的值（"wipe"）会走"不是硬切"那一支，而这一栏最后
        // 又落回 cut，于是出来一个「硬切 + 0.4 秒转场时长」的自相矛盾。
        drop_unknown_enums(item);

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
        // **进画出画的动作当场摘掉，不等崩了再补救。**
        //
        // 提示词第 6 条明令禁止（不写走进/走出/开门/镜头穿过），三集 51 镜
        // 实测把这类写法从 3/3 压到 1/18 —— 但没清零，而残留的那一条照样崩：
        // 2026-09-16 的 ep04_sh009「曾老板走向门口」五秒里换成了另一间屋子
        // 另一个人。原来只在闸门判出「片中硬切」之后才摘（render.cpp），
        // 可那道闸门量的是相邻帧差、只抓得住硬切；sh009 是**渐变漂移**，
        // 一帧一帧慢慢морф过去，闸门放行了。
        //
        // 试过给闸门补一条判据：首尾 SSIM 和首尾亮度差都量过，好坏区间完全
        // 重叠（好 0.24/0.51 对崩 0.07~0.18；亮度差好 25.9/31.4 落在崩
        // 16.2~57.4 中间）——全局帧统计分不开「人在动」和「场景换了」。
        // 与其凑一个会误伤的阈值，不如在这儿就让它没得选：规矩是明写的，
        // 违规的当场摘掉。和 drop_placeholder_dialogue、diversify_shot_sizes
        // 一样，都是「模型写错了但不该让整条流水线挂掉」。
        //
        // 摘完只剩时间码的原样留着（见 defuse_motion）：空的运动描述模型
        // 同样自由发挥，而且连线索都没有了。
        item["motion_prompt"] = defuse_motion(str_or(item, "motion_prompt"));
        // 运动描述短一截的补满，理由见 cover_full_duration。
        // **要在 duration_s 吸附之后**：补的是吸附后那个真时长。
        // 也要在摘完之后：摘掉一句会让末段的时间码落到别处。
        cover_full_duration(item);
        drop_placeholder_dialogue(item);
        // 剥旁白要在删占位之后：「（无台词）」不带引号，两步互不干扰，
        // 但顺序反过来会让剥出来的空串被当成一句真台词留下。
        clean_dialogue_text(item);
        add_missing_speakers(item, known_chars);
        link_location(item, known_locs);

        // **最后一道：只认允许它填的那些字段。** 见 keep_llm_fields。
        keep_llm_fields(item);

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
    // 景别塌成一个值的兜底，见 diversify_shot_sizes。
    diversify_shot_sizes(shots);
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
        // **冒号前 ≤12 字就算说话人**，这是这套文本格式唯一的判据——
        // 渲染出来的台词是「名字：台词」，动作行是光秃秃一行，一旦动作行
        // 自己带了冒号，两者就分不开了。
        //
        // 2026-09-13 量过：walk_c 四集剧本共 22 条冒号行，**认不出说话人的
        // 0 条**（正片路径上模型一直老实用注册角色名）。唯一一次踩到是在
        // 预告片那条路：「黑屏前最后一帧：林浩抬头望向镜头……」——冒号前
        // 七个字，于是整句动作描写被当成一个人在说话，落成旁白后被念出来。
        //
        // **那一条是在源头挡的**（strip_camera_prefix 的词表补了后期/转场
        // 术语），没有动这里的 12。理由：真台词的说话人受 schema 枚举约束、
        // 一定是注册角色，而这个阈值收紧多少才既挡住标签又不误伤，
        // 手上只有一次观察，不够。要动它得先有一批样本。
        if (text::utf8_len(name) > 12) continue;
        // **正文这条路也要清一道。** strip_list_marker / strip_speech_tags
        // 跑在「模型 JSON → 拍子」那一步，管不到已经存下的剧本：老项目、
        // 人手改过的、以及修这条之前生成的那些，正文里的 `-` 会原样变成
        // 台词，再原样进字幕。这里是「剧本正文 → 台词数据」的唯一入口，
        // 堵在这儿，三种来源一次覆盖。
        //
        // check_coverage / missing_dialogue_lines 和落位用的是同一个函数，
        // 所以清洗只能放在这里面——放外面两边就对不上了。
        const std::string said =
            strip_list_marker(text::strip_ws(line.substr(at + sep)));
        // 「林浩：（脚步声）」这种整句提示不是台词，别落成一句要念的话。
        if (!said.empty() && !is_stage_direction(said)) {
            out.emplace_back(name, said);
        }
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
    // **换完档要把运动描述跟着改。**
    //
    // 上面这个循环为了凑总时长把镜头从 5 秒换成 2 秒，而 motion_prompt 还
    // 写着 `[0-5秒]`——解析时 cover_full_duration 对的是换档**之前**那个数。
    // 2026-09-16 实测：新排的一集里 18 镜有 9 镜对不上，最离谱的一个 6 秒
    // 镜头挂着 `[0-15秒]`。多出来那截没人描述，出片模型自由发挥，而它发挥
    // 的方式就是把主体丢掉（见 cover_full_duration 上面那段）。
    //
    // 放在这儿而不是各调用方各写一遍：改时长的是这个函数，忘不掉。
    for (Shot& s : shots) {
        if (s.motion_prompt.empty()) continue;
        s.motion_prompt = motion_covering(s.motion_prompt, s.duration_s);
    }
    return shots;
}

}  // namespace changji::stages
