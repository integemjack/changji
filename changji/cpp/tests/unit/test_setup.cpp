// 首次运行那一页：模型清单、推荐、写回配置、进度解析。
//
// Python 侧没有对应物（那边模型是 ComfyUI 管的），所以不是对拍，是新逻辑。
//
// 盯五件事，每一件都是"错了不会立刻报错"的那一类：
//
//   **清单本身自洽**——角色名必须在 models_field 那张表里认得出来。
//   漏一个的表现是"下完了但配置里没写上"，而那要到出片时才报"模型没配"。
//
//   **推荐永远给得出答案**——界面上"没有默认值"等于让用户自己去猜。
//
//   **换家族要清空上一家的键**——`video_lora` 的默认值指着 H3 的 Turbo
//   LoRA，选了 Wan 之后还留着的话，sd.cpp 会把一个给 H3 做的 LoRA 挂到
//   Wan 上。加载不上而已，画面照出、耗时照旧，没有任何报错。
//
//   **aria2 的进度行**——解析错了的表现是进度条一直停在 0，而下载本身
//   是好的，很难往解析上想。
//
//   **配齐了没有**——判错的两个方向都很难看：判松了用户进首页点什么都跑
//   不了，判紧了用云端大模型的人被永远挡在这一页上。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "config/settings.hpp"
#include "http/setup_api.hpp"
#include "setup/catalog.hpp"
#include "setup/downloader.hpp"
#include "setup/source.hpp"

#include "scoped_env.hpp"

using namespace changji;
using json = nlohmann::json;

TEST_CASE("清单里每个角色名都要能落到 [models] 的某个字段上") {
    config::ModelsConfig m;
    for (const auto& g : setup::catalog()) {
        for (const auto& role : g.owned_roles) {
            CAPTURE(g.key);
            CAPTURE(role);
            CHECK(http::models_field(m, role) != nullptr);
        }
        for (const auto& o : g.options) {
            for (const auto& f : o.files) {
                if (f.role.empty()) continue;
                CAPTURE(o.id);
                CAPTURE(f.role);
                // 文件声明的角色必须是这一组管的那几个之一。不是的话
                // 写回时不会被清空，换选项之后会留在配置里。
                const bool owned = std::find(g.owned_roles.begin(),
                                             g.owned_roles.end(),
                                             f.role) != g.owned_roles.end();
                CHECK(owned);
                CHECK(http::models_field(m, f.role) != nullptr);
            }
        }
    }
}

TEST_CASE("每个文件都有仓库、路径和核对过的字节数") {
    // **字节数是下载完成的唯一判据。** 填 0 的话那个文件永远"下完了"，
    // 而截断的权重加载时报的是"读不对"，指向完全错误的方向。
    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) {
                CHECK(o.files.empty());
                continue;
            }
            // **判据是"下不下文件"，不是"是不是 kNoneOption"。**
            // 走云端 API 那一项也一个文件都不下，它不是"什么都不装"，
            // 是一个完整可用的选择——见 catalog.cpp 里 glm-4.7-flash-api。
            if (o.files.empty()) continue;
            CAPTURE(o.id);
            for (const auto& f : o.files) {
                CAPTURE(f.name);
                CHECK_FALSE(f.name.empty());
                CHECK_FALSE(f.repo.empty());
                CHECK_FALSE(f.path.empty());
                CHECK(f.bytes > 0);
                // 仓库名形如 `组织/名字`，路径不带前导斜杠——
                // 两个源的地址都是这么拼的，错了就是 404，
                // 而 404 在下载器眼里和"网络不通"长得一样。
                CHECK(f.repo.find('/') != std::string::npos);
                CHECK(f.path.front() != '/');
            }
            CHECK(o.total_bytes() > 0);
        }
    }
}

TEST_CASE("下载地址：国内魔搭、国外 HuggingFace") {
    // 三个源的仓库名和路径完全一致，只有域名和分支名不同
    // （魔搭是 master 而且多一段 /models）。拼错了就是 404。
    const std::string repo = "QuantStack/Qwen-Image-Edit-2509-GGUF";
    const std::string path = "Qwen-Image-Edit-2509-Q6_K.gguf";
    CHECK(setup::resolve_url(setup::Source::ModelScope, repo, path) ==
          "https://modelscope.cn/models/QuantStack/Qwen-Image-Edit-2509-GGUF/"
          "resolve/master/Qwen-Image-Edit-2509-Q6_K.gguf");
    CHECK(setup::resolve_url(setup::Source::HuggingFace, repo, path) ==
          "https://huggingface.co/QuantStack/Qwen-Image-Edit-2509-GGUF/"
          "resolve/main/Qwen-Image-Edit-2509-Q6_K.gguf");
    CHECK(setup::resolve_url(setup::Source::HfMirror, repo, path) ==
          "https://hf-mirror.com/QuantStack/Qwen-Image-Edit-2509-GGUF/"
          "resolve/main/Qwen-Image-Edit-2509-Q6_K.gguf");
    // 带子目录的路径原样接上，不做任何转义
    CHECK(setup::resolve_url(setup::Source::ModelScope, "Comfy-Org/MiniMax-H3",
                             "vae/minimax_h3_audio_vae_fp32.safetensors") ==
          "https://modelscope.cn/models/Comfy-Org/MiniMax-H3/resolve/master/"
          "vae/minimax_h3_audio_vae_fp32.safetensors");
    // Auto 还没探的时候当魔搭用——调用方本该先 detect_source()，
    // 真漏了的话给一条能用的路比拼出个空串好。
    CHECK(setup::resolve_url(setup::Source::Auto, repo, path) ==
          setup::resolve_url(setup::Source::ModelScope, repo, path));
}

TEST_CASE("源的名字认得出来，认不出的返回空") {
    CHECK(setup::source_from_string("modelscope") == setup::Source::ModelScope);
    CHECK(setup::source_from_string("huggingface") == setup::Source::HuggingFace);
    CHECK(setup::source_from_string("hf-mirror") == setup::Source::HfMirror);
    CHECK(setup::source_from_string("auto") == setup::Source::Auto);
    // **认不出必须是空，不能悄悄退回默认值**：前端传了个拼错的源，
    // 静默换成别的会让人以为自己选的生效了。
    CHECK_FALSE(setup::source_from_string("魔塔").has_value());
    CHECK_FALSE(setup::source_from_string("").has_value());
    // 转回去要能对上，前端拿这个字符串当选择器的值
    for (const auto s : {setup::Source::ModelScope, setup::Source::HuggingFace,
                         setup::Source::HfMirror, setup::Source::Auto}) {
        CHECK(setup::source_from_string(setup::to_string(s)) == s);
    }
}

TEST_CASE("选项 id 在整张表里唯一") {
    // 重了的话 `Group::find` 会挑到先出现的那个，而界面上两项长得不一样。
    std::set<std::string> seen;
    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) continue;  // 每组一个，本来就重
            CAPTURE(o.id);
            CHECK(seen.insert(o.id).second);
        }
    }
}

TEST_CASE("推荐：显存多大都得给得出一套") {
    for (const double vram : {0.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0, 48.0, 80.0}) {
        CAPTURE(vram);
        const auto rec = setup::recommend(vram);
        for (const auto& g : setup::catalog()) {
            CAPTURE(g.key);
            const auto it = rec.find(g.key);
            REQUIRE(it != rec.end());
            // **不能推荐"不下载"。** 那一项是给人手动挑的，
            // 推荐成默认值等于默认什么都不装。
            CHECK(it->second != setup::kNoneOption);
            CHECK(g.find(it->second) != nullptr);
        }
    }
}

TEST_CASE("推荐：卡越大挑得越好，而且不会推荐装不下的") {
    // **别把这两个叫 small / big。** Windows SDK 的 rpcndr.h 里有
    // `#define small char`，`const auto small` 在 MSVC 上直接编不过
    // （C3530「auto 不能与任何其他类型说明符组合」，指向的行看着完全正常）。
    const auto small_card = setup::recommend(8.0);
    const auto big_card = setup::recommend(80.0);
    CHECK(small_card.at("image") != big_card.at("image"));

    for (const auto& g : setup::catalog()) {
        const auto* picked = g.find(setup::recommend(24.0).at(g.key));
        REQUIRE(picked != nullptr);
        CAPTURE(g.key);
        CAPTURE(picked->id);
        // 编剧那一组的预算只有六成，别的按整张卡算
        CHECK(picked->min_vram_gb <= (g.key == "llm" ? 24.0 * 0.6 : 24.0));
    }
    const auto rec5090 = setup::recommend(31.8);
    // **推荐 = 这张卡上权重能常驻的最好一档。** 5090 上 Qwen-Image-Edit
    // Q6_K 正好是那一档：实测权重常驻 35 秒一张，再往上 Q8_0（21.8 GB）
    // 在这张卡上只能放内存，慢五倍。
    CHECK(rec5090.at("image") == "qwen-image-edit-2509-q6_k");
    // 出片这一组同理：完整版 H3 的权重要 33.4 GB 才常驻得下，这张卡差
    // 一点点，所以推荐落在精简版上。**完整版没被禁掉**，只是不当默认值
    // ——用户这台机器上现在跑的就是完整版（权重放内存，实测 124 秒一镜），
    // 那个选择由配置里的「正在用」保住，不会被推荐值顶掉。
    CHECK(rec5090.at("video") == "h3-pruned-q4_k_m");
    // **编剧那一组一律推云端那一项，跟卡多大没关系**（2026-09-13 改的）。
    // 本地跑得动的最好一档在 EQ-Bench 长文创作榜上是 59 分，而它要占走
    // 整张卡；glm-4.7-flash 的 API 不要钱，卡全留给出图出片。
    // 理由写在 config::LLMConfig::backend 上。
    for (const double vram : {8.0, 24.0, 31.8, 80.0}) {
        CAPTURE(vram);
        CHECK(setup::recommend(vram).at("llm") == "zhipu-free");
    }
    // **本地那几档 2026-09-14 整个去掉了。** 进程内后端删了之后没有任何
    // 东西会去用那份 GGUF——留在清单里只会让人下二十个 G 然后发现用不上。
    const auto* llm_group = &setup::catalog().front();
    REQUIRE(llm_group->key == "llm");
    CHECK(llm_group->find("qwen3-14b-q4_k_m") == nullptr);
    CHECK(llm_group->find("qwen3-32b-q8_0") == nullptr);
    // 这一组照样是必选：编剧模型是流水线第一步，只是现在一定是外接的
    CHECK(llm_group->required);
    for (const auto& o : llm_group->options) {
        CAPTURE(o.id);
        CHECK(o.files.empty());        // 一个字节都不用下
    }
}

TEST_CASE("显存门槛是从引擎那两个函数反推的，不是手填的") {
    // **这条钉的是"表和引擎不许分家"。** 界面上写着"≥ 24 GB 权重可常驻"，
    // 而真正决定权重放哪的是 `ModelsConfig::weights_for` /
    // `image_weights_for`。手填一份的话，那两个函数里的常数一调
    // （5090 上量出来的 14.6 / 6.6 + 4，已经改过好几轮），
    // 这张表就开始骗人——**而"说装得下、实际 OOM"没有任何报错**，
    // 只会在跑到第 34 段时炸。
    config::ModelsConfig m;
    for (const auto& g : setup::catalog()) {
        if (g.key != "video" && g.key != "image") continue;  // 大模型那条另算
        const bool image = g.key == "image";
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) continue;
            // 扩散模型是这一组文件里挂在主角色上的那一个
            double model_gb = 0;
            for (const auto& f : o.files) {
                if (f.role == g.owned_roles.front()) {
                    model_gb = static_cast<double>(f.bytes) / 1e9;
                }
            }
            REQUIRE(model_gb > 0);
            CAPTURE(o.id);
            CAPTURE(model_gb);
            CAPTURE(o.min_vram_gb);
            const auto at = [&](double v) {
                return image ? m.image_weights_for(v, model_gb)
                             : m.weights_for(v, model_gb);
            };
            // 门槛上：权重常驻。门槛下半档：退回全放内存。
            CHECK(at(o.min_vram_gb) != "cpu");
            CHECK(at(o.min_vram_gb - 0.5) == "cpu");
        }
    }
}

TEST_CASE("每个家族的每一档精度都在表里") {
    // 用户 2026-09-10 的原话："将所有精度的模型都显示出来"。
    // 少一档不会有任何报错，只是那一档在界面上不存在。
    std::map<std::string, std::set<std::string>> by_family;
    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) continue;
            CAPTURE(o.id);
            CHECK_FALSE(o.family.empty());
            CHECK_FALSE(o.family_note.empty()); // 界面按家族显示这一句
            // **量化档和显存门槛只有本地权重才有。** 云端那一项没有权重，
            // 也就没有"哪个精度""要多少显存"可言——它的门槛正好是 0，
            // 这也是 recommend() 在任何一张卡上都挑得动它的原因。
            if (o.files.empty()) continue;
            CHECK_FALSE(o.quant.empty());       // 每一档都要说清是哪个精度
            CHECK(o.min_vram_gb > 0);
            by_family[o.family].insert(o.quant);
        }
    }
    // QuantStack 那个仓库的 13 档。**这一族没有 BF16 也没有 fp8**——
    // 上游只放了 GGUF 梯队，2026-09-15 从基础版 Qwen-Image 换过来时
    // 那两档就没了（基础版在 city96 有 14 档 + Comfy 的 fp8）。
    CHECK(by_family.at("Qwen-Image-Edit 2509").size() == 13);
    // QuantStack 的 13 档量化 + Comfy 的 fp16
    CHECK(by_family.at("Wan 2.2 TI2V-5B").size() == 14);
    // 编剧模型那一组现在一个权重都不下（进程内后端删了），所以这儿
    // 不再有 Qwen3-* 那几家。
}

TEST_CASE("写回配置：选中的键填上，同一组没用到的键清空") {
    // H3 用 video_llm 当编码器、还要 video_audio_vae；
    // Wan 用 video_text_encoder，两个都不要。
    const json h3 = setup::config_patch({{"video", "h3-full-q4_k_m"}});
    CHECK(h3["models"]["video"] == "minimax_h3_fl2va-Q4_K_M.gguf");
    CHECK(h3["models"]["video_llm"] == "qwen3vl_32b_minimax_h3-Q4_K_M.gguf");
    CHECK(h3["models"]["video_audio_vae"] ==
          "minimax_h3_audio_vae_fp32.safetensors");
    CHECK(h3["models"]["video_text_encoder"] == "");
    CHECK(h3["models"]["video_high_noise"] == "");
    // H3 的三个旋钮和 Wan 完全不同，填错了都不报错
    CHECK(h3["models"]["video_rng"] == "cpu");
    CHECK(h3["models"]["video_cfg"] == 1.0);

    const json wan = setup::config_patch({{"video", "wan22-ti2v-5b-fp16"}});
    CHECK(wan["models"]["video"] == "wan2.2_ti2v_5B_fp16.safetensors");
    CHECK(wan["models"]["video_text_encoder"] == "umt5_xxl_fp16.safetensors");
    // **这一条是这组用例的重点。** video_lora 的默认值指着 H3 的
    // Turbo LoRA；不清空的话它会被挂到 Wan 上——加载不上而已，
    // 画面照出、耗时照旧，没有任何报错。
    CHECK(wan["models"]["video_lora"] == "");
    CHECK(wan["models"]["video_llm"] == "");
    CHECK(wan["models"]["video_audio_vae"] == "");
    CHECK(wan["models"]["video_cfg"] == 6.0);
}

TEST_CASE("写回配置：步数一律写 0，交给引擎按 Turbo 和档位表自己算") {
    // **写死步数会把首帧一起钉死。** `[tiers].final_steps` 会落进档位表
    // （Runtime::profile），而首帧的步数取的就是档位表那个数
    // （effective_spec 里 frame_steps = table_final_steps）。Turbo 那个 LoRA
    // 只挂在视频模型上，出图那一步没有它——6 步就是裸跑 6 步，首帧糊，
    // 而首帧是跨镜头一致性的锚点，糊了后面每一镜都糊，全程不报错。
    //
    // 2026-09-10 在这一版上真栽过：设置页从「出片 6 Turbo · 首帧 30」
    // 变成了「出片 6 Turbo · 首帧 6」。
    for (const char* id : {"h3-full-q4_k_m", "h3-pruned-q4_k_m",
                           "wan22-ti2v-5b-fp16", "wan22-ti2v-5b-q4_k_m"}) {
        CAPTURE(id);
        const json patch = setup::config_patch({{"video", id}});
        REQUIRE(patch["tiers"].contains("final_steps"));
        CHECK(patch["tiers"]["final_steps"] == 0);
    }
}

TEST_CASE("写回配置：不下载那一档只写旋钮，不动模型路径") {
    const json patch = setup::config_patch({{"llm", setup::kNoneOption}});
    CHECK(patch["llm"]["backend"] == "remote");
    // 用户可能本来就手配了一套模型，只是这次不想重下。
    CHECK_FALSE(patch.contains("models"));
}

TEST_CASE("写回配置：云端那一项连地址和模型名一起写，也不动模型路径") {
    const json patch = setup::config_patch({{"llm", "zhipu-free"}});
    CHECK(patch["llm"]["backend"] == "remote");
    CHECK(patch["llm"]["base_url"] == "https://open.bigmodel.cn/api/paas/v4");
    CHECK(patch["llm"]["model"] == "glm-4.7-flash");
    // **它不是 kNoneOption，但一个文件都不下**，所以同样不该清 models.llm
    // ——盘上那个权重还在，清了的话用户想切回本地得重新去找它叫什么。
    CHECK_FALSE(patch.contains("models"));
}

TEST_CASE("写回配置：认不出的选项跳过那一组，不炸") {
    const json patch = setup::config_patch(
        {{"video", "这个选项早就删了"}, {"image", "qwen-image-edit-2509-q6_k"}});
    CHECK_FALSE(patch["models"].contains("video"));
    CHECK(patch["models"]["image"] == "Qwen-Image-Edit-2509-Q6_K.gguf");
}

TEST_CASE("apply_setup_patch 把 patch 落到内存里那份配置上") {
    config::Settings s;
    http::apply_setup_patch(s, setup::config_patch({{"video", "h3-full-q4_k_m"}}));
    CHECK(s.models.video == "minimax_h3_fl2va-Q4_K_M.gguf");
    CHECK(s.models.video_llm == "qwen3vl_32b_minimax_h3-Q4_K_M.gguf");
    CHECK(s.models.video_rng == "cpu");
    CHECK(s.models.video_cfg == doctest::Approx(1.0));
    // 0 = 没填，按显存推的档位表走。见上面那条用例。
    CHECK(s.tiers.final_steps == 0);

    // 再换成 Wan，上一家的键必须真的被清掉——只清 patch 不清内存的话，
    // 界面显示的是新的、跑的是旧的。
    http::apply_setup_patch(s, setup::config_patch({{"video", "wan22-ti2v-5b-fp16"}}));
    CHECK(s.models.video_llm.empty());
    CHECK(s.models.video_lora.empty());
    CHECK(s.models.video_cfg == doctest::Approx(6.0));

    http::apply_setup_patch(s, json{{"llm", {{"backend", "remote"}}}});
    CHECK(s.llm.backend == "remote");

    // **地址和模型名也要落进内存，不能只写文件。** 只落 backend 的话，
    // 选了云端那一项之后文件里是智谱、内存里还是上一家，写剧本仍然发往
    // 上一家，要等重启才"自己好了"。
    http::apply_setup_patch(s, setup::config_patch({{"llm", "zhipu-free"}}));
    CHECK(s.llm.base_url == "https://open.bigmodel.cn/api/paas/v4");
    CHECK(s.llm.model == "glm-4.7-flash");
}

TEST_CASE("现在用的是哪一项：外接大模型按地址认，不按模型名") {
    // 2026-09-14 的 bug 就在这条判据上。按模型名认的话，用户在设置页把
    // glm-4.7-flash 换成 glm-5.3 那一刻，这一组就"认不出是哪一家"：
    // 初始化页显示成「不下载 · 用别的外接服务」，而下一次保存会把
    // zhipu-free 那一项写死的 glm-4.7-flash 冲回配置文件——
    // **他自己挑的模型名存不住**。
    const auto* llm = &setup::catalog().front();
    REQUIRE(llm->key == "llm");

    config::Settings s;
    s.llm.backend = "remote";
    s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4";

    SUBCASE("就是那一项默认的模型名") {
        s.llm.model = "glm-4.7-flash";
        CHECK(http::current_option(*llm, s) == "zhipu-free");
    }
    SUBCASE("换成这家别的模型，还是这一家") {
        s.llm.model = "glm-5.3";
        CHECK(http::current_option(*llm, s) == "zhipu-free");
    }
    SUBCASE("地址结尾多个斜杠不算两家") {
        s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4/";
        s.llm.model = "glm-5.3";
        CHECK(http::current_option(*llm, s) == "zhipu-free");
    }
    SUBCASE("别处的服务：认不出是哪一家，那就是「用别的外接服务」") {
        s.llm.base_url = "http://127.0.0.1:11434/v1";
        // 模型名碰巧和清单里那个一样也不算——地址才是身份证
        s.llm.model = "glm-4.7-flash";
        CHECK(http::current_option(*llm, s) == setup::kNoneOption);
    }
    // 「进程内那条路按文件认」那一支去掉了：那条后端已经没有了。
}

TEST_CASE("配齐了没有：接外面的服务也算配齐") {
    const auto* llm = &setup::catalog().front();
    REQUIRE(llm->key == "llm");

    config::Settings s;
    // **进程内那条删了，配着 local 的老配置一律不算配齐**——让人回到
    // 这一页把它改成外接，比放他过去然后第一次写剧本才炸要好。
    s.llm.backend = "local";
    CHECK_FALSE(http::group_satisfied(*llm, s));

    // **这一条挡住的是"用云端大模型的人被永远关在初始化页上"。**
    // 本机/局域网的服务不校验密钥，填不填都算配齐。
    s.llm.backend = "remote";
    s.llm.base_url = "http://127.0.0.1:11434/v1";
    s.llm.api_key = "";
    CHECK(http::group_satisfied(*llm, s));

    // **默认那一档（OpenRouter）没填密钥就不算配齐。** 只看 backend
    // 的话这一页会放人过去，然后第一次写剧本 401。
    s.llm = config::LLMConfig{};
    REQUIRE(s.llm.api_key.empty());
    CHECK_FALSE(http::group_satisfied(*llm, s));
    s.llm.api_key = "sk-or-v1-填了";
    CHECK(http::group_satisfied(*llm, s));

    // 改回 local 照样不算配齐，不管别的填成什么样
    s.llm.backend = "local";
    CHECK_FALSE(http::group_satisfied(*llm, s));
}

TEST_CASE("aria2 的进度行认得出来") {
    const auto p = setup::parse_aria2_progress(
        "[#396de5 3.7MiB/2.3GiB(0%) CN:8 DL:844KiB ETA:48m2s]");
    REQUIRE(p.has_value());
    CHECK(p->downloaded == static_cast<std::uint64_t>(3.7 * 1024 * 1024));
    CHECK(p->total == static_cast<std::uint64_t>(2.3 * 1024 * 1024 * 1024));
    CHECK(p->speed_bps == doctest::Approx(844.0 * 1024));
}

TEST_CASE("aria2 的进度：取最后一条，不是第一条") {
    // 日志是追加的，前面那些是几秒钟以前的。取第一条的表现是
    // 进度条永远停在开跑那一瞬间，而下载本身在跑。
    const std::string log =
        "[#396de5 3.7MiB/2.3GiB(0%) CN:8 DL:844KiB ETA:48m2s]\n"
        "FILE: /tmp/probe.gguf\n"
        "[#396de5 1.2GiB/2.3GiB(52%) CN:8 DL:12MiB ETA:1m30s]\n";
    const auto p = setup::parse_aria2_progress(log);
    REQUIRE(p.has_value());
    CHECK(p->downloaded == static_cast<std::uint64_t>(1.2 * 1024 * 1024 * 1024));
    CHECK(p->speed_bps == doctest::Approx(12.0 * 1024 * 1024));
}

TEST_CASE("aria2 的进度：不是进度行的一概返回空") {
    // 返回一个"0 字节"比返回空更糟：那会把进度条按回 0，
    // 看着像下载重来了。
    CHECK_FALSE(setup::parse_aria2_progress("").has_value());
    CHECK_FALSE(setup::parse_aria2_progress("Download Results:").has_value());
    CHECK_FALSE(setup::parse_aria2_progress("[#396de5 没有斜杠]").has_value());
}

TEST_CASE("人读的字节数换算") {
    CHECK(setup::parse_human_bytes("512") == 512);
    CHECK(setup::parse_human_bytes("1KiB") == 1024);
    CHECK(setup::parse_human_bytes("1.5MiB") == 1024 * 1024 * 3 / 2);
    CHECK(setup::parse_human_bytes(" 2GiB") ==
          static_cast<std::uint64_t>(2) * 1024 * 1024 * 1024);
    CHECK_FALSE(setup::parse_human_bytes("abc").has_value());
    CHECK_FALSE(setup::parse_human_bytes("").has_value());
}

TEST_CASE("进度快照能转成 JSON，字段一个都不少") {
    // 前端整页都靠这几个字段。少一个的表现是界面上某一处永远是
    // "undefined"，而接口本身 200。
    setup::Snapshot s;
    setup::ItemProgress p;
    p.group = "video";
    p.option = "h3-full-q4_k_m";
    p.name = "a.gguf";
    p.total = 100;
    p.downloaded = 40;
    p.speed_bps = 1024;
    p.state = setup::ItemState::Running;
    s.items.push_back(p);
    s.total = 100;
    s.downloaded = 40;
    s.state = setup::RunState::Running;

    const json j = s.to_json();
    CHECK(j["state"] == "running");
    CHECK(j["total"] == 100);
    CHECK(j["downloaded"] == 40);
    CHECK(j["items"][0]["state"] == "running");
    CHECK(j["items"][0]["speedBps"] == 1024.0);
    for (const char* key : {"state", "items", "error", "tool", "dir", "total",
                            "downloaded", "speedBps", "etaSeconds"}) {
        CAPTURE(key);
        CHECK(j.contains(key));
    }
}

TEST_CASE("下到一半的模型不能算齐——aria2c 会把文件先按最终大小占好") {
    // 2026-09-10 真栽在这上面：61.7 GB 的 bf16 才下了 6 GB，而 aria2c
    // 一开始就把整个文件预分配了，于是 file_size 返回的**正好是**清单里
    // 那个字节数。"大小对上 = 下完了"这条被骗过去，初始化页显示已完成，
    // 用户选了它去出片——花屏。而且一句报错都没有：safetensors 的头在
    // 文件开头，早下下来了，解析得好好的，错的是后面还是零的那些张量。
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp =
        fs::temp_directory_path() / "changji_dl_test";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const fs::path model = tmp / "big.safetensors";
    { std::ofstream f(model, std::ios::binary); f << "xxxx"; }

    SUBCASE("没有控制文件：不在下载中") {
        CHECK_FALSE(http::download_in_progress(model));
    }
    SUBCASE("有 .aria2 控制文件：还在下，哪怕大小已经对上") {
        fs::path ctrl = model;
        ctrl += ".aria2";
        { std::ofstream f(ctrl, std::ios::binary); f << "ctrl"; }
        CHECK(http::download_in_progress(model));
    }
    SUBCASE("文件压根不存在也不算在下载") {
        CHECK_FALSE(http::download_in_progress(tmp / "根本没有这个.gguf"));
    }
    fs::remove_all(tmp, ec);
}

TEST_CASE("同时下几个：按下载器分档，能用环境变量压回去") {
    using changji::setup::parallel_lanes;

    // curl 是单连接，并发几个就快几倍——这一档给得多。
    CHECK(parallel_lanes(10, "curl") == 4);
    // aria2c 自己已经开了 8 条连接（`-x 8`）。再乘 4 就是 32 条，
    // 源站那边要么限速要么掐连接，所以这一档只给 2。
    CHECK(parallel_lanes(10, "aria2c") == 2);

    // **不能超过要下的文件数**：三个文件开四路，多出来的那一路
    // 一开起来就发现没活干，白建一个线程。
    CHECK(parallel_lanes(3, "curl") == 3);
    CHECK(parallel_lanes(1, "curl") == 1);
    // 一个文件都没有时也得是 1：返回 0 的话下面那个线程池一路都不起，
    // 整轮下载会"成功"地什么都没下。
    CHECK(parallel_lanes(0, "curl") == 1);

    // 压回一个一个下，以及封顶。环境变量写坏了按默认来，不让整轮停下。
    {
        const changji::test::ScopedEnv one("CHANGJI_DOWNLOAD_PARALLEL", "1");
        CHECK(parallel_lanes(10, "curl") == 1);
    }
    {
        const changji::test::ScopedEnv many("CHANGJI_DOWNLOAD_PARALLEL", "99");
        CHECK(parallel_lanes(50, "curl") == 8);   // 封在 8
    }
    {
        const changji::test::ScopedEnv junk("CHANGJI_DOWNLOAD_PARALLEL", "很多");
        CHECK(parallel_lanes(10, "curl") == 4);   // 认不出就按默认
    }
}
