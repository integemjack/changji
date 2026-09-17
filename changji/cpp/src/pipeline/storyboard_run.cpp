#include "pipeline/storyboard_run.hpp"

#include <algorithm>

#include "stages/storyboard.hpp"
#include "util/text.hpp"

namespace changji::pipeline {

using namespace changji::models;

StoryboardRunResult run_storyboard(const StoryboardRunOptions& opts,
                                   llm::Client& client, CancelToken& tok) {
    StoryboardRunResult result;
    const std::string& script = opts.script;
    const AssetLibrary& assets = opts.assets;

    std::vector<stages::SceneBlock> scenes = stages::split_scenes(script, assets);
    std::vector<Shot> shots;
    // **目标量按剧本估，不按名义时长。** 估不出来（剧本是空的、全是场次头）
    // 才退回 duration_s。
    const double estimated = stages::estimate_script_seconds(script);
    const double target_s = estimated > 0.0 ? estimated : opts.duration_s;

    if (scenes.size() <= 1) {
        // ---- 整集一次拆：和 2026-09-15 之前逐字节一样 ----
        const stages::DurationQuota quota =
            stages::DurationQuota::for_duration(target_s);
        llm::Request req;
        req.prompt = stages::build_storyboard_prompt(script, assets, quota,
                                                     opts.episode_id);
        // 镜头数写进 schema。配额那句话模型不一定听——实测 60 秒的集出过
        // 两镜六秒，提示词里"合计 16 个镜头"一个字没少。
        req.schema = stages::llm_shot_schema(
            assets, stages::shot_count_bounds(quota, target_s,
                                              stages::count_beats(script)));
        req.schema_name = "storyboard";
        req.on_thinking = opts.on_thinking;
        // **让它少想一格。** 2026-09-17 把这一步的思考捞出来读了：它在思考
        // 里把整张分镜表逐镜写完了（Shot 11 的 first_frame / dialogue /
        // motion / characters 全是成品），然后才用 JSON 再写一遍——同一份
        // 东西写两遍，而这一步是全流水线最贵的：一集七八分钟，采样下来思考
        // 每秒涨约 190 字，一场跑了 410 秒还一个字正文都没落。
        //
        // 智谱的 reasoning_effort **默认是 max**，我们一直没说话。这儿往下
        // 一格到 high，不是一步到 low——拆分镜要挑景别、挑运镜，不是纯转写。
        // 认不得这个参数的模型一个字段都不多发（见 build_payload）。
        //
        // 撤掉就是删这一行。真觉得镜头变差了，先撤这一行再谈别的。
        req.reasoning_effort = "high";
        if (opts.peek) {
            result.peeked = llm::schema_as_prompt(req.prompt, req.schema);
            return result;
        }
        shots = stages::parse_storyboard(
            opts.pasted.empty() ? client.complete(req, tok) : opts.pasted.front(),
            assets);
    } else {
        // ---- 按场拆 ----
        stages::assign_scene_seconds(scenes, target_s);
        result.scenes = static_cast<int>(scenes.size());
        std::string prev_tail;
        int running_order = 0;
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            const stages::SceneBlock& scene = scenes[i];
            if (opts.on_progress) {
                opts.on_progress("正在拆第 " + std::to_string(scene.index) + "/" +
                                 std::to_string(scenes.size()) + " 场" +
                                 (scene.body.empty() ? "" : "：" + scene.body));
            }
            const stages::DurationQuota quota =
                stages::DurationQuota::for_duration(scene.seconds);
            llm::Request req;
            req.prompt = stages::build_scene_storyboard_prompt(
                scene, static_cast<int>(scenes.size()), assets, quota,
                opts.episode_id, prev_tail);
            req.schema = stages::llm_scene_shot_schema(
                assets,
                stages::shot_count_bounds(quota, scene.seconds,
                                          stages::count_beats(scene.text)),
                scene.location_id,
                // 第几场。拆完 `stamp_scene` 盖的就是这个数，提前告诉模型，
                // 省得它为一个会被覆盖的值猜半天。
                std::max(1, scene.index));
            req.schema_name = "storyboard";
            req.on_thinking = opts.on_thinking;
            // 同上面整集那条，理由写在那儿。
            req.reasoning_effort = "high";
            if (opts.peek) {
                // 场次头写清楚是哪一场：三份提示词贴到别处去跑，得认得出
                // 哪份对哪份。
                result.peeked += "\n\n===== 第 " + std::to_string(scene.index) +
                                 "/" + std::to_string(scenes.size()) + " 场" +
                                 (scene.body.empty() ? "" : "：" + scene.body) +
                                 " =====\n\n" +
                                 llm::schema_as_prompt(req.prompt, req.schema);
                continue;
            }

            // 粘回来的那一段，或者去问模型。**数量在进来之前就核过**
            //（见 post_plan）：少一段的话后面几场整体错位一场，而错位
            // 出来的分镜表看着是合法的，没有任何报错。
            const std::string raw = opts.pasted.empty()
                                        ? client.complete(req, tok)
                                        : opts.pasted.at(i);
            std::vector<Shot> part = stages::parse_storyboard(raw, assets);
            stages::stamp_scene(part, scene);
            // 各场的 order 都从 0 起，合起来之前先排成全集的次序，
            // 不然最后重编号那一步按 order 稳定排序会把几场交错在一起。
            std::stable_sort(part.begin(), part.end(),
                             [](const Shot& a, const Shot& b) {
                                 return a.order < b.order;
                             });
            for (Shot& s : part) s.order = running_order++;
            const Shot& last = part.back();
            prev_tail = !text::strip_ws(last.visual_desc).empty()
                            ? last.visual_desc
                            : last.first_frame_prompt;
            shots.insert(shots.end(), part.begin(), part.end());
        }
        // **只看不发到此为止。** 底下那几道（补台词、查覆盖、重编号）都是
        // 对着真镜头做的，零镜头走过去会报"分镜表不完整"——而这一下本来
        // 就没打算生成任何镜头。
        if (opts.peek) return result;
    }

    // **先把剧本里漏掉的台词补进去，再查。** 分镜模型不搬台词——实跑
    // 九句只写两句，dump 出来看是压根没生成。台词本来就在剧本里，
    // 有顺序有说话人，引擎自己放比指望模型重打一遍靠谱。
    //
    // 放在 check_coverage 之前：一句台词都没写的那种「哑剧」，本来就
    // 是这一步能救回来的，不该先报错退出。
    result.placed_lines = stages::place_missing_dialogue(shots, script, assets);
    // 口型要在补完台词之后推，否则补进去的那几镜不会做口型。
    const auto gaps = stages::check_coverage(script, shots);
    if (!gaps.empty()) {
        std::string msg = "分镜表不完整：";
        for (const auto& g : gaps) msg += "\n" + g;
        msg += "\n\n换一个更强的模型，或者手工补齐这些字段后再跑。";
        throw stages::StoryboardError(msg);
    }
    apply_lipsync_rules(shots);
    // 编号和顺序按引擎的来。模型编出来的 id 有错集号、没补零、打错字的。
    stages::renumber_shots(shots, opts.episode_id);
    // **不压回目标时长。** 这一章多长由它自己的内容定，装配时再按每集
    // 时长切成几集。2026-09-16 之前这儿有一道 rebalance_durations，
    // 只在集模式下跑——那条路当天删了，这一道跟着没了。
    result.shots = std::move(shots);
    return result;
}

}  // namespace changji::pipeline
