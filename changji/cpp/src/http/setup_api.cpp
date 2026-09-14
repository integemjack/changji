#include "http/setup_api.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "config/runtime.hpp"
#include "config/writeback.hpp"
#include "setup/downloader.hpp"
#include "setup/source.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

using setup::catalog;
using setup::Group;
using setup::Item;
using setup::kNoneOption;
using setup::Option;

bool download_in_progress(const fs::path& p) {
    std::error_code ec;
    fs::path ctrl = p;
    ctrl += ".aria2";
    return fs::exists(ctrl, ec);
}

namespace {

std::uint64_t size_of(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}



/// 这个目录（或者它最近的一个存在的上级）所在盘还剩多少。
///
/// **要往上找**：模型目录多半还不存在——它正是这一页要建的那个。
/// 对一个不存在的路径调 fs::space 拿到的是错误码，界面上就成了"剩 0 字节"，
/// 而那会让人以为盘满了。
std::pair<std::uint64_t, std::uint64_t> disk_space(fs::path dir) {
    std::error_code ec;
    while (!dir.empty()) {
        if (fs::exists(dir, ec)) {
            const auto s = fs::space(dir, ec);
            if (!ec) {
                return {static_cast<std::uint64_t>(s.available),
                        static_cast<std::uint64_t>(s.capacity)};
            }
        }
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return {0, 0};
}

json option_json(const Option& o, const fs::path& models_dir, double vram_gb) {
    json files = json::array();
    std::uint64_t have = 0;
    bool complete = !o.files.empty();
    for (const auto& f : o.files) {
        const fs::path full = models_dir / paths::from_utf8(f.name);
        const std::uint64_t on_disk = size_of(full);
        // 还在下就一律不算齐，哪怕大小已经对上了（预分配，见上面）。
        const bool present =
            f.bytes > 0 && on_disk == f.bytes && !download_in_progress(full);
        if (present) have += f.bytes;
        // 下了一半的也算上。**预分配的那种算不出来**：文件已经是最终大小，
        // 真下了多少只有 .aria2 里的位图知道。那种情况这里记 0，
        // 界面上进度偏小——比显示"已完成"好，后者会让人拿一份零文件去出片。
        else if (on_disk > 0 && on_disk < f.bytes) have += on_disk;
        if (!present) complete = false;
        files.push_back({{"name", f.name},
                         {"note", f.note},
                         {"bytes", f.bytes},
                         {"haveBytes", on_disk},
                         {"present", present}});
    }
    return {{"id", o.id},
            {"family", o.family},
            {"label", o.label},
            {"quant", o.quant},
            {"familyNote", o.family_note},
            {"note", o.note},
            // **这个数是"权重常驻得下的显存"，不是"跑不起来的下限"。**
            // 界面上必须这么措辞：低于它照样能跑，只是权重放内存、
            // 每一步都在等 PCIe。写成"最低要求"会让 16 GB 的用户
            // 以为自己什么都跑不了。
            {"minVramGb", o.min_vram_gb},
            // fits 只是"推荐不推荐"，**不禁止选**：卡小但内存大的机器
            // 把权重放内存照样跑得动，只是慢。挡住不如把代价说清楚。
            {"fits", o.min_vram_gb <= vram_gb},
            {"totalBytes", o.total_bytes()},
            {"haveBytes", have},
            {"complete", complete},
            {"files", files}};
}

/// 两个接口地址是不是同一家。只差结尾的斜杠不算两家。
std::string same_service_key(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

}  // namespace

std::string current_option(const Group& g, const config::Settings& s) {
    // **远端那条路不按文件认。** 切到云端时我们特意没清 [models].llm
    // （见 catalog.cpp 的 config_patch），所以上次下的那个权重还在配置里；
    // 按文件认的话这一页会选中本地那一档，而实际跑的是云端——界面说的
    // 和真跑的不是一回事，比不显示更糟。
    //
    // ⚠️ **按地址认这一家，不是按模型名。** 2026-09-14 用户报「智谱的
    // 模型名保存不了」，根子就在这儿：原来是拿 `llm.model` 和每一项写死
    // 的那个名字比，而模型名恰恰是用户在设置页自己挑的东西。他把
    // glm-4.7-flash 换成 glm-5.3 那一刻，这一组就"认不出是哪一家"了，
    // 于是
    //   * 这一页显示成「不下载 · 用别的外接服务」——他明明在用智谱；
    //   * 更糟的是下面 post_setup_download 的判据跟着塌：选中项从
    //     `zhipu-free` 变成 `none`，页面上摆着的还是 `zhipu-free`，
    //     一保存就被当成"换了一家"，把那一项写死的 glm-4.7-flash
    //     冲回配置文件。
    // 一家服务 = 一个地址。模型名归用户，这一页不拿它当身份证。
    if (g.key == "llm" && s.llm.backend != "local") {
        const auto here = same_service_key(s.llm.base_url);
        for (const auto& o : g.options) {
            for (const auto& [key, value] : o.settings) {
                if (key == "llm.base_url" && value.is_string() &&
                    same_service_key(value.get<std::string>()) == here) {
                    return o.id;
                }
            }
        }
        // 认不出是哪一家（自己填的地址）：那就是"用别的外接服务"那一项。
        return kNoneOption;
    }
    if (g.owned_roles.empty()) return {};
    config::ModelsConfig copy = s.models;
    const std::string* primary = models_field(copy, g.owned_roles.front());
    if (primary == nullptr || primary->empty()) return {};
    for (const auto& o : g.options) {
        for (const auto& f : o.files) {
            if (f.role == g.owned_roles.front() && f.name == *primary) return o.id;
        }
    }
    return {};
}

namespace {

/// 写回配置并让内存里那份跟上。
void persist(const json& patch) {
    if (patch.empty()) return;
    config::save_user_config(patch);
    auto s = config::runtime().snapshot();
    apply_setup_patch(s, patch);
    config::runtime().replace(s);
}

}  // namespace

std::string* models_field(config::ModelsConfig& m, const std::string& role) {
    // 一张平表而不是一串 if：漏一个角色的表现是"下完了但配置里没写上"，
    // 而那要到出片时才报"模型没配"，离这里隔着十万八千里。
    // 加字段时这里也要加——test_setup.cpp 拿 catalog 里出现过的角色查这张表。
    // **成员指针类型先起个别名。** 直接把 `std::string config::ModelsConfig::*`
    // 写进模板实参表里，MSVC 解析不了（error C2059，然后 kMap 整个没声明成）；
    // GCC 接受，所以只在 Windows 上编不过。别名两边都认。
    using Field = std::string config::ModelsConfig::*;
    static const std::map<std::string, Field> kMap = {
        {"llm", &config::ModelsConfig::llm},
        {"video", &config::ModelsConfig::video},
        {"video_high_noise", &config::ModelsConfig::video_high_noise},
        {"video_vae", &config::ModelsConfig::video_vae},
        {"video_text_encoder", &config::ModelsConfig::video_text_encoder},
        {"video_llm", &config::ModelsConfig::video_llm},
        {"video_llm_vision", &config::ModelsConfig::video_llm_vision},
        {"video_audio_vae", &config::ModelsConfig::video_audio_vae},
        {"video_lora", &config::ModelsConfig::video_lora},
        {"image", &config::ModelsConfig::image},
        {"image_vae", &config::ModelsConfig::image_vae},
        {"image_text_encoder", &config::ModelsConfig::image_text_encoder},
        {"image_text_encoder_vision",
         &config::ModelsConfig::image_text_encoder_vision},
        {"tts", &config::ModelsConfig::tts},
        {"tts_decoder", &config::ModelsConfig::tts_decoder},
    };
    const auto it = kMap.find(role);
    return it == kMap.end() ? nullptr : &(m.*(it->second));
}

bool group_satisfied(const Group& g, const config::Settings& s) {
    // 编剧和配音有另一条出路：接外面的服务。那时候本机一个文件都没有
    // 也算配齐——不认这一条的话，用云端大模型的人会被永远挡在这一页上。
    //
    // **但云端那条还要有密钥才算配齐。** 2026-09-13 默认改成了远端的
    // glm-4.7-flash，装完就是"backend=remote、api_key 空"这个状态；
    // 只看 backend 的话这一页会说"配好了"放人过去，然后第一次写剧本
    // 401。本机/局域网的服务不要求——Ollama 那些根本不校验。
    if (g.key == "llm" && s.llm.backend != "local") {
        return !s.llm.needs_api_key() || !s.llm.api_key.empty();
    }
    if (g.key == "tts" && s.tts.backend != "local") return true;
    if (g.owned_roles.empty()) return true;

    const fs::path ws = s.workspace_path();
    config::ModelsConfig copy = s.models;
    // 主角色配了、而且文件真的在，才算这一组能用。
    // **只看配没配是不够的**：配了一个不存在的文件名是最常见的情形
    // （手抄配置抄错、模型没下完），而那时候出片会在跑到一半时炸。
    const std::string* primary = models_field(copy, g.owned_roles.front());
    if (primary == nullptr || primary->empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(s.models.resolve(*primary, ws), ec);
}

void apply_setup_patch(config::Settings& s, const json& patch) {
    if (patch.contains("models") && patch["models"].is_object()) {
        for (const auto& [key, value] : patch["models"].items()) {
            if (std::string* field = models_field(s.models, key)) {
                if (value.is_string()) *field = value.get<std::string>();
                continue;
            }
            // 这一页会写的几个非路径旋钮。H3 和 Wan 的这几个数不一样，
            // 而填错了三处都不报错——出来的片和提示词没关系而已。
            if (key == "dir" && value.is_string()) {
                const auto v = value.get<std::string>();
                s.models.dir = v.empty() ? std::optional<std::string>()
                                         : std::optional<std::string>(v);
            } else if (key == "video_rng" && value.is_string()) {
                s.models.video_rng = value.get<std::string>();
            } else if (key == "video_lora_tiers" && value.is_string()) {
                s.models.video_lora_tiers = value.get<std::string>();
            } else if (key == "video_cfg" && value.is_number()) {
                s.models.video_cfg = value.get<double>();
            } else if (key == "video_flow_shift" && value.is_number()) {
                s.models.video_flow_shift = value.get<double>();
            } else if (key == "video_lora_strength" && value.is_number()) {
                s.models.video_lora_strength = value.get<double>();
            }
        }
    }
    if (patch.contains("tiers") && patch["tiers"].is_object()) {
        const auto& t = patch["tiers"];
        if (t.contains("final_steps") && t["final_steps"].is_number_integer()) {
            s.tiers.final_steps = t["final_steps"].get<int>();
        }
        if (t.contains("draft_steps") && t["draft_steps"].is_number_integer()) {
            s.tiers.draft_steps = t["draft_steps"].get<int>();
        }
    }
    // **这三项要一起落。** 原来只落 backend，于是选了云端那一项之后，
    // 文件里是智谱的地址和模型，内存里还是上一家的——写剧本仍然发往
    // 上一家，而且要等到重启才"自己好了"。反过来同样难查：2026-09-14
    // 那个 bug 里，这一页把 llm.model 冲回默认只改了文件没改内存，
    // 于是界面上一切正常，下次开机才跳回去。
    if (patch.contains("llm") && patch["llm"].is_object()) {
        const auto& l = patch["llm"];
        if (l.contains("backend") && l["backend"].is_string()) {
            s.llm.backend = l["backend"].get<std::string>();
        }
        if (l.contains("base_url") && l["base_url"].is_string()) {
            s.llm.base_url = l["base_url"].get<std::string>();
        }
        if (l.contains("model") && l["model"].is_string()) {
            s.llm.model = l["model"].get<std::string>();
        }
    }
    if (patch.contains("tts") && patch["tts"].is_object() &&
        patch["tts"].contains("backend") && patch["tts"]["backend"].is_string()) {
        s.tts.backend = patch["tts"]["backend"].get<std::string>();
    }
}

ApiResult get_setup_state(const config::Settings& settings,
                          const models::HardwareProfile& profile) {
    // **探源放在这儿而不是下载那一步。** 探一次要两个 HEAD，最坏 12 秒；
    // 放在下载请求里的话，用户点了「开始下载」之后要愣十几秒才看到动静，
    // 而那时候他不知道程序在干什么。放在这儿是页面打开时顺手探的，
    // 而且探完的结果要显示出来。进程内只探一次，见 detect_source()。
    const setup::SourceProbe probe = setup::probe_sources();
    // 探测结果单独一个字段，别拼进说明里——拼的话"连不上"后面会跟一个
    // "秒"，成了「连不上 秒」。前端只负责把它排出来。
    const auto probe_label = [&probe](double v) {
        if (probe.how != "probed") return std::string();
        if (v < 0) return std::string("连不上");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f 秒", v);
        return std::string(buf);
    };
    const std::string probe_ms = probe_label(probe.modelscope_s);
    const std::string probe_hf = probe_label(probe.huggingface_s);

    const fs::path ws = settings.workspace_path();
    const fs::path models_dir = settings.models.dir_path(ws);
    // **按物理显存挑，不按 vram_gb_override。** 那个数是拿来挑画质档位的，
    // 和这张卡实际有多少显存是两回事——2026-09-10 混用它的后果是
    // fp8 图像模型在第 34/62 段 OOM。
    const double vram_gb =
        profile.gpu.has_value() ? profile.gpu->vram_gb() : profile.vram_gb;

    const auto recommended = setup::recommend(vram_gb);

    json groups = json::array();
    json selected = json::object();
    bool needed = false;
    for (const auto& g : catalog()) {
        const bool ok = group_satisfied(g, settings);
        if (g.required && !ok) needed = true;

        json options = json::array();
        for (const auto& o : g.options) options.push_back(option_json(o, models_dir, vram_gb));

        // 界面上先选中的那一项：已经配着的优先，没有就用推荐的。
        // **顺序不能反**——反过来的话，用户上次特意挑了小一档的模型，
        // 这一页会把它换回推荐的那档，而他多半不会注意到。
        const std::string current = current_option(g, settings);
        const auto rec = recommended.find(g.key);
        selected[g.key] = !current.empty()
                              ? current
                              : (rec == recommended.end() ? std::string() : rec->second);

        groups.push_back({{"key", g.key},
                          {"title", g.title},
                          {"purpose", g.purpose},
                          {"required", g.required},
                          {"satisfied", ok},
                          {"options", options}});
    }

    const auto [free_bytes, total_bytes] = disk_space(models_dir);

    json gpu = nullptr;
    if (profile.gpu.has_value()) {
        gpu = {{"name", profile.gpu->name},
               {"vramGb", profile.gpu->vram_gb()},
               {"count", profile.gpu->count}};
        // **统一内存的机器上，整机多大也要告诉界面。**
        // 这一页上所有的门槛都是拿 vramGb 比的（"≥ 81 GB 可常驻"），而在
        // 苹果芯片上那是 Metal 肯给的那一份（128 GB 的机器上 107.5 GB），
        // 不是整机内存。只报前者，用户看到的是"我买的明明是 128"——
        // 而这一页正是他决定要不要下 91 GB 那一档的地方。
        // 不是统一内存时这一项是 null，界面就只显示一个数。
        gpu["unifiedGb"] = profile.gpu->unified()
                               ? json(static_cast<double>(profile.gpu->unified_mb) / 1024.0)
                               : json(nullptr);
    }

    return {200,
            {{"needed", needed},
             {"detected", profile.detected},
             {"gpu", gpu},
             {"vramGb", vram_gb},
             {"modelsDir", paths::to_utf8(models_dir)},
             {"configFile", paths::to_utf8(config::user_config_path())},
             {"diskFreeBytes", free_bytes},
             {"diskTotalBytes", total_bytes},
             // 一个下载器都没有的话，界面要在按下载之前就说出来，
             // 而不是让人点完等半天才看到一句"起不来 aria2c"。
             {"tool", setup::pick_tool()},
             // **从哪儿下。国内魔搭、国外 HuggingFace。**
             // 探出来的那个只是默认值，界面上可以改——探测本身会错：
             // 挂了代理的国内机器 HuggingFace 可能更快，而反过来，
             // 公司网络里魔搭也可能被挡。所以探完要把依据一起报出去，
             // 让用户看得见"为什么是这个源"。
             {"source", setup::to_string(probe.source)},
             {"sourceHow", probe.how},
             {"sources",
              json::array({json{{"id", "modelscope"},
                                {"label", "魔搭 ModelScope"},
                                {"note", "国内"},
                                {"probe", probe_ms}},
                           json{{"id", "huggingface"},
                                {"label", "HuggingFace"},
                                {"note", "国外"},
                                {"probe", probe_hf}},
                           json{{"id", "hf-mirror"},
                                {"label", "HuggingFace 镜像"},
                                {"note", "魔搭上万一缺某个文件时的退路，不参与自动挑选"},
                                {"probe", ""}}})},
             {"recommended", recommended},
             {"selected", selected},
             {"groups", groups},
             {"download", setup::Downloader::instance().snapshot().to_json()}}};
}

ApiResult post_setup_download(const config::Settings& settings, const json& body) {
    // **只保存配置，不下文件。** 用户 2026-09-14："模型没下载也应该可以
    // 保存，提示用户是否现在下载。"
    //
    // 这两件事本来就该分开：「这一档是我要的」是个决定，「文件到盘上了」
    // 是件体力活。捆在一起的后果是——挑一档 40 GB 的权重，就得先等它下完
    // 才算选上，中途一停配置还回到原样。
    //
    // 默认仍是 true，老的调用方一个字都不用改。
    const bool want_download = [&] {
        const auto it = body.find("download");
        return it == body.end() || !it->is_boolean() || it->get<bool>();
    }();

    if (setup::Downloader::instance().running()) {
        // 409 而不是静默忽略：用户点了第二次而界面什么都没变的话，
        // 他会以为第一次没点上。
        // **只保存也拦**：下载器正拿着上一套选择在跑，这会儿把配置改成
        // 另一套，下完那一下 on_item_done 又会写回去，两边打架。
        throw ApiError(409, "已经在下了。要换选择先点停止。");
    }
    // **这一条只在真要下的时候问。** 机器上没装 aria2/curl 跟"我想把配置
    // 存下来"毫无关系，而拦在这儿的话，没装下载器的机器连模型都选不了。
    if (want_download && setup::pick_tool().empty()) {
        throw ApiError(400,
                       "这台机器上没找到下载器（aria2c 或 curl）。"
                       "装一个再回来：Debian/Ubuntu 是 apt-get install -y aria2，"
                       "Windows 是 winget install aria2.aria2。");
    }

    // 从哪儿下。前端不传就用探出来的那个。
    setup::Source source = setup::detect_source();
    if (const auto raw = body.find("source");
        raw != body.end() && raw->is_string() && !raw->get<std::string>().empty()) {
        const auto picked = setup::source_from_string(raw->get<std::string>());
        // **认不出就报错，不静默退回默认值**：用户选了个源而程序偷偷换掉，
        // 他会以为自己选的生效了，然后对着"怎么还是这么慢"发愣。
        if (!picked.has_value()) {
            throw ApiError(400, "不认识的下载源：" + raw->get<std::string>());
        }
        if (*picked != setup::Source::Auto) source = *picked;
    }

    const auto it = body.find("selections");
    if (it == body.end() || !it->is_object()) throw ApiError(400, "缺 selections");
    std::map<std::string, std::string> selections;
    for (const auto& [key, value] : it->items()) {
        if (value.is_string()) selections[key] = value.get<std::string>();
    }
    if (selections.empty()) throw ApiError(400, "一组都没选");

    // 模型放哪。用户填了就以填的为准，**而且要先写进配置再开下**——
    // 否则下完之后配置里的相对路径仍然相对老目录解析，文件在盘上却"找不到"。
    fs::path models_dir = settings.models.dir_path(settings.workspace_path());
    json immediate = json::object();
    if (const auto dir = body.find("dir");
        dir != body.end() && dir->is_string() && !dir->get<std::string>().empty()) {
        const auto raw = dir->get<std::string>();
        models_dir = fs::absolute(paths::expand_user(raw));
        immediate["models"]["dir"] = paths::to_utf8(models_dir);
    }

    // **有没有哪一组真的换了。** 下面用它决定"写哪些组"，理由见那段注释。
    bool any_changed = false;
    for (const auto& g : catalog()) {
        const auto pick = selections.find(g.key);
        if (pick == selections.end()) continue;
        if (current_option(g, settings) != pick->second) {
            any_changed = true;
            break;
        }
    }

    std::vector<Item> items;
    for (const auto& g : catalog()) {
        const auto pick = selections.find(g.key);
        if (pick == selections.end()) continue;
        const Option* opt = g.find(pick->second);
        if (opt == nullptr) throw ApiError(400, "不认识的选项：" + pick->second);
        const bool changed = current_option(g, settings) != pick->second;

        // **每一组选中的配置都立刻写，不管它要不要下文件。**
        //
        // 原来只有 `files.empty()` 那几组（云端 API、"不下载"）在这儿写，
        // 要下文件的那些等 on_item_done 在整组下完之后才写。那样一来
        // "选中"和"下完"是同一件事，中途一停就什么都没留下。
        //
        // ⚠️ 下面那个 on_item_done 的老注释说「一组的文件全齐了才写」，
        // 防的是**半组**：video 指着新模型、video_vae 还是上一档，两个
        // 文件都在盘上却不配套，sd.cpp 不报错只出一段花屏。**那个顾虑
        // 和这里不冲突**——这里写的是整组，一个文件都还没下。配置指着
        // 盘上没有的文件是个**说得出口**的状态：体检里那条"配了 N 项、
        // 其中 M 项缺"会直接点出来（doctor.cpp 的 configured/missing）。
        // 静默的花屏和明说的缺文件，不是一码事。
        //
        // ⚠️ **没动过的组不要重写。** 2026-09-14 用户报「智谱的模型名又
        // 保存不了了」——真相是：他在「大模型」那一节把 llm.model 改成
        // glm-5.3 存好了，接着点了这一页的保存，而 llm 这一组的选中项
        // 仍然是 `zhipu-free`，那一项的 settings 里**写死着
        // `llm.model = glm-4.7-flash`**，于是把他刚存的模型名冲回默认。
        // 运行时内存里还是新值，所以当场看不出来，**下次读配置文件才跳
        // 回去**——最难查的那种。
        //
        // 判据是"这一组的选中项变了没有"，不是"这一页点没点保存"：
        // 他压根没碰这一组，我们就不该动它写下的任何一项。
        // （这条判据本身还要 current_option 认得出这一家才算数——
        //  那正是它按地址认、不按模型名认的原因，见上面那段。）
        //
        // **一组都没变时反而全写**：那正是按钮显示「重写配置」的那一下，
        // 用途就是配置手改坏了拿它修回来。少了这一支，那个功能会变成
        // 一个什么都不做的按钮。
        if (!opt->settings.empty() && (changed || !any_changed)) {
            json patch = setup::config_patch({{g.key, opt->id}});
            // **已经在这家了就别动模型名。** 上面那条「一组都没变时全写」
            // 是留给「重写配置」的，但它会连 `llm.model` 一起重写成这一项
            // 写死的那个默认值——而模型名恰恰是用户在「大模型」那一节自己
            // 挑的，不是这一页管的东西。判据是地址没变：地址一样就说明他
            // 还在这家，没换服务商，那模型名归他。
            if (!changed && patch.contains("llm") && patch["llm"].is_object() &&
                patch["llm"].contains("base_url") &&
                patch["llm"]["base_url"].is_string() &&
                same_service_key(patch["llm"]["base_url"].get<std::string>()) ==
                    same_service_key(settings.llm.base_url)) {
                patch["llm"].erase("model");
            }
            for (const auto& [section, values] : patch.items()) {
                if (!values.is_object()) {
                    immediate[section] = values;  // 不在任何小节里的顶层项
                    continue;
                }
                for (const auto& [k, v] : values.items()) immediate[section][k] = v;
            }
        }
        // 判据是"有没有文件"，不是"是不是 kNoneOption"：走云端 API
        // 那一项（zhipu-free）也一个文件都不下，但它带着三个必须写的旋钮。
        if (opt->files.empty()) continue;
        for (const auto& f : opt->files) {
            items.push_back(
                {g.key, opt->id, f, setup::resolve_url(source, f.repo, f.path)});
        }
    }

    try {
        persist(immediate);
    } catch (const std::exception& e) {
        throw ApiError(500, std::string("配置写不进去：") + e.what());
    }

    if (!want_download || items.empty()) {
        // 两种情况回同一个形状，因为对调用方来说是同一件事：**没起下载**。
        //   * `download:false` —— 只保存。配置上面那次 persist 已经写进去
        //     了，要下什么由前端问过用户再说。
        //   * items 为空 —— 全选了"不下载"，或者选的都已经在盘上。
        // 两者都不算错。
        return {200, {{"started", false},
                      {"progress", setup::Downloader::instance().snapshot().to_json()}}};
    }

    // 每下完一个文件回来一次。**一组的文件全齐了才写那一组的配置**：
    // 只写一半的话，配置里 video 指着新模型、video_vae 还是上一档的，
    // 而这种组合 sd.cpp 不报错，只是出一段花屏。
    const auto on_item_done = [](const Item& done) {
        const auto snap = setup::Downloader::instance().snapshot();
        for (const auto& p : snap.items) {
            if (p.group != done.group || p.option != done.option) continue;
            if (p.state != setup::ItemState::Done && p.state != setup::ItemState::Present) {
                return;  // 这一组还没齐
            }
        }
        try {
            persist(setup::config_patch({{done.group, done.option}}));
        } catch (const std::exception&) {
            // 写不进去不该把下载也停掉——文件是有用的，
            // 配置用户还能自己填。真正的报错留给下一次 state 请求：
            // 那时候这一组会显示成"文件都在，但配置没指过去"。
        }
    };

    setup::Downloader::instance().start(std::move(items), models_dir,
                                       setup::to_string(source), on_item_done);
    return {200, {{"started", true},
                  {"progress", setup::Downloader::instance().snapshot().to_json()}}};
}

ApiResult get_setup_progress() {
    return {200, setup::Downloader::instance().snapshot().to_json()};
}

ApiResult post_setup_cancel() {
    setup::Downloader::instance().cancel();
    return {200, setup::Downloader::instance().snapshot().to_json()};
}

}  // namespace changji::http
