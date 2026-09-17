#include <algorithm>
#include "http/batch.hpp"

#include "http/story_api.hpp"
#include "config/runtime.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "http/job_stream.hpp"
#include "http/episodes.hpp"
#include "http/scripting.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "stages/bible.hpp"
#include "models/story.hpp"
#include "http/ws.hpp"
#include "stages/chapter_write.hpp"
#include "stages/json_stream.hpp"
#include "stages/script.hpp"
#include "stages/story_plan.hpp"
#include "pipeline/storyboard_run.hpp"
#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

void forbid_extra(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw ApiError(400, "请求体要是一个对象");
    for (const auto& kv : body.items()) {
        // **`stream` 一律放行。** 它是传输层的信封字段，不是业务字段：
        // 路由那一层（script_route / batch_route）拿它决定这件活挪不挪到
        // 后台、结果往哪条 WebSocket 送，处理函数多半根本不看它。
        //
        // 原来是各家自己往白名单里加，2026-09-14 栽了：给「照故事定妆」
        // 接上思考流之后前端开始发 stream，而 post_bible 的白名单里没有，
        // 一按就是 422 `Extra inputs are not permitted`。十几个处理函数
        // 挨个加，漏一个的表现就是那一步整个不能用。
        //
        // `async` 不在这儿放行是因为它在更上面就被 take_async 摘掉了
        // （见 server.cpp），到这儿本来就没有。
        if (kv.key() == "stream") continue;
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(kv.key(), "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

std::string need_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end()) {
        // **input 是整个请求体，不是 null。** FastAPI 报缺字段时把父对象
        // 放进 input，前端拿它回显"你提交的是这些"。写 null 的话那一栏是空的。
        // 实时对拍抓出来的（写接口那一轮）。
        throw unprocessable_top(key, "Field required", body, "missing");
    }
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    return it->get<std::string>();
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

double num_in_range(const json& body, const char* key, double def, double gt,
                    double le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number()) {
        throw unprocessable_top(key, "Input should be a valid number", *it,
                                "float_type");
    }
    const double v = it->get<double>();
    // 边界值用 bound_text 印，不用 std::to_string——后者给六位小数，
    // 1800.0 会变成 "1800.000000"，而 pydantic 的消息里是 "1800"。
    // 而且要带 ctx，前端靠它填出"最大 1800"这种中文提示。
    if (v <= gt) {
        throw out_of_range(key, "Input should be greater than " + bound_text(gt),
                           *it, "greater_than", "gt", gt);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + bound_text(le), *it,
            "less_than_equal", "le", le);
    }
    return v;
}

int int_in_range(const json& body, const char* key, int def, int ge, int le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number_integer()) {
        throw unprocessable_top(key, "Input should be a valid integer", *it,
                                "int_type");
    }
    const int v = it->get<int>();
    if (v < ge) {
        throw out_of_range(
            key, "Input should be greater than or equal to " + std::to_string(ge),
            *it, "greater_than_equal", "ge", ge);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + std::to_string(le),
            *it, "less_than_equal", "le", le);
    }
    return v;
}

ProjectStore open_project(const json& body) {
    const std::string path = need_str(body, "project");
    if (path.empty()) throw ApiError(400, "没有指定项目目录");
    return ProjectStore(paths::from_utf8(path));
}

Project load_or_400(const ProjectStore& store) {
    try {
        return store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

std::vector<std::string> character_names(const AssetLibrary& assets) {
    std::vector<std::string> names;
    for (const auto& kv : assets.characters) names.push_back(kv.second.name);
    return names;
}

double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

}  // namespace

ApiResult post_story_chapters(const json& body,
                              std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    // 409 在建 store 之前判，和 post_script_series 一个顺序：
    // 两个都错时回哪一个是可观测的。
    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        throw ApiError(409, "已经在写了");
    }
    ProjectStore store = open_project(body);
    // **回给界面的项目串，用客户端自己发来的那一份。**
    //
    // 不能用 `store.root()`：ProjectPaths 的构造会做 weakly_canonical
    // （见 project.cpp），而界面手里那串来自项目列表、不保证规范化过
    // ——macOS 上 /tmp 会解成 /private/tmp 这类符号链接，两串就对不上。
    // 下面那条流式正文要靠这串让界面认"这是不是我这部剧的"；对不上的话
    // 收到的正文会被整个丢掉，而且是静默的。
    const std::string client_project = need_str(body, "project");
    const Project project = load_or_400(store);

    Story story;
    try {
        story = store.load_story();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
    if (story.chapters.empty()) {
        throw ApiError(400, "还没有故事。先写一份大纲，或者粘一段进来");
    }

    std::vector<std::string> todo;
    for (const auto& c : story.chapters) {
        if (overwrite || text::strip_ws(c.text).empty()) {
            todo.push_back(c.chapter_id);
        }
    }
    if (todo.empty()) throw ApiError(400, "每一章都有正文了");

    const models::StyleLine style = project.style_line;
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo, style, client_project](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            // 推流式正文要用它。**按类订阅也收得到**：Hub 把 job_id 里第一个
            // '-' 之前的部分当类名（"write-a3f…" → "write"），而客户端只订
            // 得到类名——具体 id 从来不从任何接口暴露出去。
            const std::string job_id =
                pipeline::jobs().job_id(pipeline::JobKind::Write);
            // 思考流挂到这条 job 的频道上。**令牌借 p 那个，不能让
            // JobScope 自带一个**：循环查的是 `p.cancelled()`、给大模型的
            // 是 `p.token()`，自带那个没人读——顶栏「停下」按下去接口回
            // `{"stopped": true}`，活一秒没停。见 JobScope 第二个构造函数。
            const JobScope scope{job_id, p.token()};
            int done = 0;
            for (const auto& id : todo) {
                if (p.cancelled()) return;

                // **每一轮重读。** 上一章写完已经落库了，这一章的提示词里
                // 「上一章是这么结束的」要拿到刚写的那一份。用循环外那个
                // 快照的话，每一章都以为自己接的是空的上一章。
                Story cur = store.load_story();
                const Chapter* me = cur.chapter_by_id(id);
                if (me == nullptr) {
                    p.set_done(++done);
                    continue;
                }
                p.set_message("正在写 " + id + "（" + me->title + "）");

                llm::Request req;
                try {
                    req.prompt = stages::build_chapter_prompt(cur, id, style);
                } catch (const std::exception& e) {
                    p.add_episode(json{{"chapter_id", id}, {"error", e.what()}});
                    p.set_done(++done);
                    continue;
                }
                req.schema = stages::chapter_schema(stages::chapter_target_scenes(cur),
                                                     stages::chapter_scene_paras(cur));
                req.schema_name = "chapter";
                req.on_thinking = thinking_sink();
                req.temperature = stages::kChapterTemperature;

                // **砸了就再要一次。**
                //
                // 最常见的砸法是模型把章节标题填进了正文字段，于是正文只有
                // 十几个字（守卫按目标篇幅的两成拦下来）。采样带随机种子，
                // 再要一次通常就对了——2026-09-11 实跑四章砸了两章，而这
                // 两章的失败彼此无关。
                //
                // **只有批量这条路重试。** 单章那个接口前面站着一个人，他
                // 看见报错自己会再按一下；批量这条是十六章无人看管地跑，
                // 中间掉两章的话，等他回来时故事里有两个空洞，而进度条上
                // 写的是"写完了 16 章"。
                //
                // 就多要两次，不是要到成功为止：提示词真有毛病时，重试到底
                // 只会把一次失败变成一小时失败。
                //
                // **最后一次把软闸关掉**（parse_chapter 的 strict=false）。
                // 软闸拦的是「能用但不够好」——整章没对白、两场撞车、章尾
                // 点题。一直硬拦的话 14B 连着写不对，那一章落成 **0 字**，
                // 而 0 字比「写得一般」差得多：2026-09-12 三轮实跑里软闸
                // 每轮清掉 1~3 章。最后一次收下它，软闸就只提分不清零。
                Story next;
                std::string last_error;
                constexpr int kAttempts = 3;
                for (int attempt = 0; attempt < kAttempts; ++attempt) {
                    // 点了停就别再来一次了——重试的那一次一样会当场被取消，
                    // 白跑一趟提示词。
                    if (p.cancelled()) break;
                    if (attempt > 0) {
                        p.set_message("重写 " + id + "（" + me->title +
                                      "）——上一次只写出几个字");
                    }
                    try {
                        const int floor_chars = static_cast<int>(
                            stages::chapter_target_chars(cur) *
                            stages::kChapterMinRatio);
                        // **批量这条也边写边推。** 十六章要跑一个多小时，
                        // 进度条上只有"正在写 ch07"的话，那一个多小时里
                        // 看不到一个字。推的是从 token 流里抠出来的正文，
                        // JSON 外壳和后面那串 hooks 不推（见 json_stream）。
                        // **抠的是 paragraphs，不是 text。** c41821d 把章节正文从一个字符串
            // 改成了一段一项的数组，而这里没跟着改——于是流式一个字都抠
            // 不出来，界面上就是"AI 写作没有热更新"，后端不报任何错。
            stages::JsonFieldStreamer field(stages::kChapterBodyField, true);
                        int seq = 0;
                        // ⚠️ **令牌要给这个任务真正的那一个（p.token()）。**
                        //
                        // 这里原来是一个当场新建的 `CancelToken dummy`——
                        // 它永远不会被点亮，于是"停"这个按钮只在**章与章
                        // 之间**那一下才生效（循环头上那句 p.cancelled()）。
                        // 表现就是用户点了停，AI 还在哗哗地写，要等这一章
                        // 写完才停——32B 上那是好几分钟。
                        //
                        // JobProgress::token() 上面那段注释早就写清楚了：
                        // "sd.cpp 的采样、llama.cpp 的生成、ffmpeg 子进程
                        // 都要拿到令牌本身才能被打断"。批量这条漏了。
                        const std::string raw = client->complete(
                            req, p.token(), [&](const std::string& piece) {
                                const std::string fresh = field.feed(piece);
                                if (fresh.empty()) return;
                                // **带上项目。** 这条流广播在 "write" 这个
                                // 槽上（客户端只订得到类名），而槽是全局
                                // 的一个：界面那头常驻一条连接收它。人在
                                // 批量跑着的时候去看另一部剧，收到的还是
                                // 这一部的正文——不说清是谁的，那边就会把
                                // 它画进别人的编辑器里。章号是 ch01 这种，
                                // 两部剧都有，光靠它分不出来。
                                ws::hub().broadcast(
                                    job_id, {{"type", "story_token"},
                                             {"job_id", job_id},
                                             {"project", client_project},
                                             {"chapter_id", id},
                                             {"seq", seq++},
                                             {"text", fresh}});
                            });
                        // **接在刚读回来的那一份上，不是循环开头那份。**
                        //
                        // 上面这次生成要跑一两分钟，而这一两分钟里用户完全
                        // 可能在编辑器里改别的章——那一页就是这么设计的：
                        // 一边看 AI 写，一边还能读能改，而改完 1.5 秒自动
                        // 保存直接落 story.json。拿循环开头那份快照整份写
                        // 回去，那些字就被悄悄吞掉了，不报错，人是过几分钟
                        // 翻回去才发现。
                        //
                        // 重读一次的代价是一个文件；换来的是"丢一章手写的
                        // 正文"这种没法补救的事不会发生。
                        next = stages::apply_chapter(
                            store.load_story(), id,
                            stages::parse_chapter(raw, floor_chars,
                                                  attempt + 1 < kAttempts));
                        last_error.clear();
                        break;
                    } catch (const std::exception& e) {
                        last_error = e.what();
                    }
                    if (p.cancelled()) break;
                }
                if (!last_error.empty()) {
                    // 一章写砸了不该让前面几章白写，记下来接着往下写。
                    p.add_episode(json{{"chapter_id", id},
                                       {"error", last_error + "（重试过两次）"}});
                    p.set_done(++done);
                    continue;
                }

                next.plan = stages::plan_episodes(next, next.episode_duration_s);
                store.save_story(next);
                // 正文扩写完，这一章的梗概和它值多长都变了——剧集跟着对齐。
                // 非章模式下这一句什么都不做。
                sync_episodes_to_chapters(store, next);

                const Chapter* written = next.chapter_by_id(id);
                p.add_episode(json{
                    {"chapter_id", id},
                    {"title", written != nullptr ? written->title : std::string()},
                    {"chars", written != nullptr ? written->text_len() : 0},
                    {"episodes", next.plan.size()},
                });
                p.set_done(++done);
            }
            p.set_message("写完了 " + std::to_string(done) + " 章");
        },
        "已手动停止。已经写好的几章留着。",
        // 顶栏那块"AI 作业中"要靠它说清是哪部剧、点了往哪儿跳。
        paths::to_utf8(store.root()),
        // 任务页面那一行。**`JobKind::Write` 一个槽里跑着三种活**，
        // 不各自报名字的话那一行只会写「批量」。
        "写正文 · 还缺的 " + std::to_string(todo.size()) + " 章");

    if (!started) throw ApiError(409, "已经在写了");
    return {200, {{"started", true}, {"chapters", todo.size()}}};
}

ApiResult post_script_series(const json& body,
                             std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "premise", "episodes", "duration_s",
                        "reuse_characters"});
    const std::string premise = need_str(body, "premise");
    const int episodes = int_in_range(body, "episodes", 3, 1, 20);
    const double duration_s = num_in_range(body, "duration_s", 60.0, 0.0, 1800.0);
    const bool reuse_chars = opt_bool(body, "reuse_characters", true);

    // 409 要在**建 store 之前**判断吗？不。Python 那边是先判 running
    // 再 load_project，所以"已经在写了"优先于"项目不存在"。照抄这个顺序：
    // 两个都错时回哪一个是可观测的。
    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        throw ApiError(409, "已经在写了");
    }
    ProjectStore store = open_project(body);
    load_or_400(store);   // 只为了验证项目能读，结果不用

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, premise, episodes, duration_s,
         reuse_chars](pipeline::JobProgress& p) {
            p.set_total(episodes);
            // **思考流挂到这条 job 的频道上。** 批量这几条是全流水线上跑得
            // 最久的（一整季几十分钟），最需要"它到底在想还是卡死了"这个
            // 信号；而它们不走 start_async，所以要自己挂一次。
            // 停这一族仍然走 /api/script/series/stop（JobKind::Write 那个槽），
            // 不是按 stream——这条 job 本来就只有一个。
            // 令牌借 p 那个，理由同上面展开正文那条。
            const JobScope scope{pipeline::jobs().job_id(pipeline::JobKind::Write),
                                 p.token()};

            // 梗概先存下来。下次打开界面时回填，不用凭记忆重打。
            Project first = store.load_project();
            const std::string trimmed = text::strip_ws(premise);
            if (first.premise != trimmed) {
                first.premise = text::truncate_utf8(trimmed, 2000);
                store.save_project(first);
            }

            int done = 0;
            for (int i = 0; i < episodes; ++i) {
                if (p.cancelled()) return;

                Project project = store.load_project();
                const AssetLibrary assets = store.load_assets();
                const std::vector<std::string> names =
                    reuse_chars ? character_names(assets)
                                : std::vector<std::string>{};

                p.set_message("正在写第 " + std::to_string(i + 1) + " 集");

                // **四段的 schema，和单集那条走同一份。**
                //
                // 这儿原来用的是 `script_schema()`——**平的那份**，只有
                // minItems 4 的地板，没有时间结构。那正是 2026-09-12 之前
                // 的行为，也正是「60 秒的集写出 13 秒的剧本」的来源：
                // 提示词里说"要凑够"模型不听，minItems 写 4 它就写 4 拍。
                //
                // 2026-09-13 实跑这条路（三集，每集目标 60 秒）：
                //     ep01   7 行 /  3 句台词 / 293 字
                //     ep02  19 行 /  8 句台词 / 477 字
                //     ep03   3 行 /  1 句台词 / 122 字
                // 而同一天走单集那条出的是 17 拍 / 对白 171 字 /「合适」。
                // **当初改四段时漏了这一条路。**
                //
                // 形状每集重摇一个（random_shape，ComfyUI 的 randomize 那个
                // 意思）：写死比例的话整季每集都是同一个模子，连着看就露馅。
                //
                // **三处都要带上同一个 variation：提示词、schema、解析。**
                // 2026-09-14 发现提示词那一处漏了——它当时还不收这个参数，
                // 于是提示词里写着「开场钩子（0–5 秒）」（固定的 8%），
                // schema 里写的却是摇出来的秒数。模型照提示词写、我们照
                // schema 解析，两边差几秒，**而且不报错**。
                const std::uint32_t variation = stages::random_shape();

                llm::Request req;
                req.prompt = stages::build_script_prompt(
                    premise, duration_s, project.style_line,
                    // 前情收在 scripting.hpp 的 previous_scripts 里。
                    // **这儿原来自己抄了一份**，两份的取法还不一样：这份取
                    // "最近三集写好的"、不管在这一集前面还是后面。写整季时
                    // 两者恰好相等（这一集是写完才建出来的，见下面
                    // next_episode_id——所以"已经写好的"就是"这一集之前
                    // 的"），所以谁也没发现。终点留空就是全部已写的。
                    previous_scripts(project), names, variation);
                req.schema =
                    stages::script_schema(duration_s, names, variation);
                req.schema_name = "script";
                req.on_thinking = thinking_sink();

                stages::ScriptDraft draft;
                try {
                    // 令牌给这个任务真正的那一个，理由同上面写章节那处：
                    // 给 dummy 的话，"停"要等这一集写完才生效。
                    draft = stages::parse_script(client->complete(req, p.token()),
                                                 duration_s, variation);
                } catch (const std::exception& e) {
                    // 一集写砸了不该让前面几集白写，记下来接着往下写。
                    // episode_id 留空——这一集根本没建出来。
                    p.add_episode(json{{"episode_id", ""}, {"error", e.what()}});
                    p.set_done(++done);
                    continue;
                }

                // **写回去之前重新读一份。**
                //
                // 上面那次生成跑了一两分钟，而 `project` 是那一两分钟**之前**
                // 读的。把它整份写回去，这期间界面上改的东西全被悄悄吞掉：
                // 镜头抽屉存的那一笔、改过的集名、手动加的一集、删掉的一集。
                // 同 post_story_chapters 和 post_plan_all 那两处，理由一样。
                //
                // 集号也要按新那份算：这几分钟里手动加过一集的话，照旧那份
                // 算出来的号会撞上它。
                Project latest = store.load_project();
                const std::string episode_id = next_episode_id(latest);
                Episode ep;
                ep.episode_id = episode_id;
                ep.title = draft.title;
                ep.synopsis = draft.logline;
                ep.script = draft.render();
                ep.target_duration_s = duration_s;
                latest.episodes.push_back(std::move(ep));
                store.save_project(latest);

                p.add_episode(json{
                    {"episode_id", episode_id},
                    {"title", draft.title},
                    {"logline", draft.logline},
                    {"speakers", draft.speakers()},
                    {"dialogue_chars", draft.dialogue_chars()},
                });
                p.set_done(++done);
            }
            p.set_message("写完了 " + std::to_string(done) + " 集");
        },
        "已手动停止。已经写好的几集留着。",
        paths::to_utf8(store.root()), "写整季 · " + std::to_string(episodes) + " 集");

    if (!started) throw ApiError(409, "已经在写了");
    return {200, {{"started", true}, {"total", episodes}}};
}

/// 把所有挂着章、又还没有剧本的章一次改编完。
///
/// 用户 2026-09-16：「交互过程也太繁琐。」原来一章要点两下、等一轮：
/// 「改编成剧本」等两三分钟，回来点「采用」，下一章再来一遍——八章就是
/// 十六下点击、八段等待。而**旁边「批量补分镜」早就是一次点完**，
/// 缺的就是它前面这一步。
///
/// **不复制单章那条路的逻辑**：它要挑上一章的结尾接语气、要按章排场、
/// 章模式和老路线还分岔（见 post_script_write）。这儿原样调它，出来的稿子
/// 直接存（post_script）——批量就是"我不逐篇看了"，没有草稿态。
ApiResult post_script_all(const json& body, std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        throw ApiError(409, "剧本那边还在忙");
    }
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);

    std::vector<std::string> todo;
    for (const auto& ep : project.episodes) {
        // 挂着章的才改编：手动加的一集、预告片那种没有章可照，跳过。
        if (ep.chapter_refs.empty()) continue;
        if (!overwrite && !text::strip_ws(ep.script).empty()) continue;
        todo.push_back(ep.episode_id);
    }
    // **"没有要做的"不是错。**
    //
    // 这两颗批量按钮的用法就是"隔一阵按一下，把新写的章补上"，而按下去
    // 十有八九本来就没有漏的——原来这一下回 400，界面上弹一个带警告角标
    // 的红框，说的却是"一切正常，你不用做任何事"。用户 2026-09-16：
    // 「交互过程也太繁琐。」报错该留给真出事的时候。
    //
    // 回 200 + `started: false` + 空 `episodes`，让调用方自己挑话说。
    // 跑起来那一条仍然是 202（它是异步的）；这一条没有活要干，就是 200。
    if (todo.empty()) {
        return {200, {{"started", false}, {"episodes", json::array()}}};
    }

    const std::string root = paths::to_utf8(store.root());
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo, root](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            // 思考流和取消令牌都挂在这条 job 上，理由同 post_plan_all。
            const JobScope scope{
                pipeline::jobs().job_id(pipeline::JobKind::Write), p.token()};
            int done = 0;
            for (const std::string& episode_id : todo) {
                if (p.cancelled()) return;
                p.set_message("正在改编 " + episode_id);
                try {
                    pipeline::CancelToken& tok = p.token();
                    json req = json::object();
                    req["project"] = root;
                    req["episode_id"] = episode_id;
                    // 老路线那一支要这个键才不报"缺 premise"；章模式用不上。
                    req["premise"] = "";
                    const ApiResult wrote = post_script_write(req, *client, tok);
                    if (wrote.status != 200 || !wrote.body.is_object()) continue;
                    const auto sit = wrote.body.find("script");
                    if (sit == wrote.body.end() || !sit->is_string()) continue;

                    json save = json::object();
                    save["project"] = root;
                    save["episode_id"] = episode_id;
                    save["script"] = *sit;
                    if (const auto lit = wrote.body.find("logline");
                        lit != wrote.body.end() && lit->is_string()) {
                        save["synopsis"] = *lit;
                    }
                    post_script(save, *client, tok);
                } catch (const std::exception&) {
                    // **一章砸了不拖垮整批。** 后面那几章照跑，跑完人回来看
                    // 哪几章还是空的——和 post_plan_all 那条一个做法。
                }
                p.set_done(++done);
            }
        },
        "已手动停止。已经写好的几集剧本留着。", root,
        "写剧本 · 还缺的 " + std::to_string(todo.size()) + " 集");
    if (!started) throw ApiError(409, "剧本那边还在忙");
    return {202, {{"started", true}, {"episodes", todo}}};
}

ApiResult post_plan_all(const json& body, std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        // 和写整季不是同一句话。用户看到"已经在写了"会去找哪里在写剧本，
        // 而实际情况是那个槽被别的事占着。
        throw ApiError(409, "剧本那边还在忙");
    }
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);

    std::vector<std::string> todo;
    for (const auto& ep : project.episodes) {
        if (text::strip_ws(ep.script).empty()) continue;
        // **判据是"有能用的镜头"，不是"shots 数组非空"。**
        //
        // 空壳镜头（shot_id 是空串）不该算数：它们指不到任何文件、进不了
        // 任何一步，可数组非空就把这一章挡在补分镜之外——人看到的是
        // 「没有要补的」，而那一章明明是空的，一键跑完整部剧也救不回来。
        //
        // 正常流程产不出这种东西（2026-09-17 那个"写回空壳"的 bug 修掉了，
        // 见 stages/render.cpp 的 Done::ran）。但存盘被截断、手工改坏
        // project.json 一样能留下它，而**判据写成"有没有能用的"本来就更对**
        // ——数组长度从来不是这一章有没有分镜的答案。
        const bool has_usable =
            std::any_of(ep.shots.begin(), ep.shots.end(), [](const auto& s) {
                return !text::strip_ws(s.shot_id).empty();
            });
        if (!overwrite && has_usable) continue;
        todo.push_back(ep.episode_id);
    }
    // **"没有要做的"不是错。**
    //
    // 这两颗批量按钮的用法就是"隔一阵按一下，把新写的章补上"，而按下去
    // 十有八九本来就没有漏的——原来这一下回 400，界面上弹一个带警告角标
    // 的红框，说的却是"一切正常，你不用做任何事"。用户 2026-09-16：
    // 「交互过程也太繁琐。」报错该留给真出事的时候。
    //
    // 回 200 + `started: false` + 空 `episodes`，让调用方自己挑话说。
    // 跑起来那一条仍然是 202（它是异步的）；这一条没有活要干，就是 200。
    if (todo.empty()) {
        return {200, {{"started", false}, {"episodes", json::array()}}};
    }

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            // **思考流挂到这条 job 的频道上。** 批量这几条是全流水线上跑得
            // 最久的（一整季几十分钟），最需要"它到底在想还是卡死了"这个
            // 信号；而它们不走 start_async，所以要自己挂一次。
            // 停这一族仍然走 /api/script/series/stop（JobKind::Write 那个槽），
            // 不是按 stream——这条 job 本来就只有一个。
            // 令牌借 p 那个，理由同上面展开正文那条。**这一条尤其要紧**：
            // 批量补分镜是从设定页「分集」那一格按的，而那一页自己的提示
            // 写着「顶栏那块「AI 作业中」里看进度」——顶栏那个「停下」是
            // 它唯一看得见的出口。
            const JobScope scope{pipeline::jobs().job_id(pipeline::JobKind::Write),
                                 p.token()};
            int done = 0;
            for (const std::string& episode_id : todo) {
                if (p.cancelled()) return;
                p.set_message("正在给 " + episode_id + " 出分镜");

                Project project = store.load_project();
                AssetLibrary assets = store.load_assets();
                Episode* ep = project.episode_by_id(episode_id);
                if (ep == nullptr) continue;

                try {
                    // 令牌同上：新建一个永远不会点亮的话，点了停要等这一集
                    // 的设定整套出完才有反应。
                    pipeline::CancelToken& tok = p.token();
                    // 角色设定全剧共用，第一次缺的时候补一次就够。
                    // 每集都重出的话，同一个角色前后长得不一样。
                    if (assets.characters.empty()) {
                        llm::Request breq;
                        breq.prompt = stages::build_bible_prompt(
                            ep->script, project.style_line);
                        breq.schema = stages::bible_schema();
                        breq.schema_name = "bible";
                        breq.on_thinking = thinking_sink();
                        AssetLibrary made = stages::parse_bible(
                            client->complete(breq, tok), project.style_line,
                            config::load_settings(store.root())
                                .video.aspect_ratio());
                        // **写回之前看一眼：这一分钟里别处可能已经补上了。**
                        // 出这份圣经要跑一趟大模型，而这期间人完全可能在设
                        // 定页按了「照故事定妆」。那就用人家那份——它是照整
                        // 个故事出的，比这儿照一集剧本出的全，而且人正看着
                        // 它。反过来拿这一份顶掉，人刚定完的角色当场全换。
                        AssetLibrary latest = store.load_assets();
                        if (latest.characters.empty()) {
                            assets = std::move(made);
                            store.save_assets(assets);
                        } else {
                            assets = std::move(latest);
                        }
                    }

                    // 单镜的时长档位是这部剧的属性（[video].max_shot_s），
                    // 按项目那份设置算一遍再拆镜头。见 config::apply_video_limits。
                    config::apply_video_limits(config::load_settings(store.root()));
                    // 切场、拆镜、补台词、查覆盖、重编号、拉回时长都在
                    // run_storyboard 里，和 post_plan 是同一份。
                    pipeline::StoryboardRunOptions sb;
                    sb.script = ep->script;
                    sb.assets = assets;
                    sb.episode_id = episode_id;
                    sb.duration_s = ep->target_duration_s;
                    sb.on_thinking = thinking_sink();
                    sb.on_progress = [&p, &episode_id](const std::string& m) {
                        p.set_message(episode_id + "：" + m);
                    };
                    ep->shots = pipeline::run_storyboard(sb, *client, tok).shots;

                    // **写回之前重读一遍。** 理由同上面写整季那处：拆一集
                    // 镜头要跑几分钟，而这几分钟里界面可能在改别的集的镜头、
                    // 改集名、加一集——`project` 是循环开头那份快照，整份
                    // 写回去就把那些改动吞了。只把这一集的镜头放进新读的
                    // 那一份。
                    //
                    // 这一集在这期间被删了的话就别写了：拿旧快照写回去
                    // 等于把它从坟里刨出来。
                    Project latest = store.load_project();
                    if (Episode* target = latest.episode_by_id(episode_id)) {
                        target->shots = ep->shots;
                        store.save_project(latest);
                    }
                } catch (const std::exception& e) {
                    // 一集出错不拖垮后面几集。跑一晚上，早上发现第二集挂了
                    // 导致后面十集都没动，那这一晚上就白熬了。
                    p.add_episode(
                        json{{"episode_id", episode_id}, {"error", e.what()}});
                    p.set_done(++done);
                    continue;
                }

                // 按成片长度报，不按名义值加——见 stages::real_total_s。
                const double total = stages::real_total_s(ep->shots);
                p.add_episode(json{{"episode_id", episode_id},
                                   {"title", ep->title},
                                   {"shots", ep->shots.size()},
                                   {"duration_s", round1(total)}});
                p.set_done(++done);
            }
            p.set_message("出完了 " + std::to_string(done) + " 集的分镜");
        },
        "已手动停止。已经出好的分镜留着。",
        paths::to_utf8(store.root()),
        "补分镜 · 还缺的 " + std::to_string(todo.size()) + " 集");

    if (!started) throw ApiError(409, "剧本那边还在忙");
    return {200, {{"started", true}, {"episodes", todo}}};
}

}  // namespace changji::http
