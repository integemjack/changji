#include "setup/catalog.hpp"

#include <algorithm>
#include <cmath>

#include "config/settings.hpp"

namespace changji::setup {

using json = nlohmann::json;

namespace {

double gb(std::uint64_t bytes) { return static_cast<double>(bytes) / 1e9; }

/// 权重要多大的卡才**常驻得下**（GB）。
///
/// **不自己算，去问引擎真正在用的那个函数。** `ModelsConfig::weights_for` /
/// `image_weights_for` 里那几个常数（视频的 14.6 GB 计算缓冲、图像的
/// 6.6 + 4 GB 再乘 0.9）都是 5090 上量出来的，而且改过好几轮。这张表要是
/// 照抄一份，公式一调它就开始骗人——而"界面说装得下、实际 OOM"这种错
/// 不会有任何报错，只会在跑到第 34 段时炸。
///
/// 反推的办法是扫：从 4 GB 往上按 0.5 GB 一档试，找它从 "cpu"
/// （＝权重全放内存）翻成组件规格的那一点。整张表就扫这么几十次，一次性的。
double resident_vram(bool image, double model_gb) {
    config::ModelsConfig m;  // weights / image_weights 默认都是 "smart"
    for (double v = 4.0; v <= 200.0; v += 0.5) {
        const std::string w =
            image ? m.image_weights_for(v, model_gb) : m.weights_for(v, model_gb);
        if (w != "cpu") return v;
    }
    return 200.0;
}

/// 大模型（llama.cpp）要多大的卡。
///
/// 这条没有对应的引擎函数可问——llama.cpp 是整个载进显存的，没有
/// "放内存流式跑"那一档。所以只能估，锚点是这台机器上量到的一个数：
/// Qwen3-14B Q4_K_M 文件 9.0 GB，载进去占 15.4 GB（多出来的是 KV 缓存
/// 和上下文）。1.25 倍加 4 GB 对得上那个点（9×1.25+4 = 15.25）。
///
/// **估偏了的代价不对称**：估低了用户挑一个装不下的，llama.cpp 直接报错
/// 载不进去；估高了只是推荐保守一档。所以宁可往高了估。
double llm_vram(double model_gb) {
    return std::ceil((model_gb * 1.25 + 4.0) * 2.0) / 2.0;
}

/// 这一档量化本身怎么样。**同一把尺子量所有家族**，省得每处各写一句
/// 而且互相打架。
std::string quant_note(const std::string& q) {
    if (q == "bf16" || q == "fp16" || q == "BF16" || q == "F16") {
        return "原始精度，不损失任何东西。体积也是最大的。";
    }
    if (q == "fp8") return "半精度再对折，画质接近原始精度。";
    if (q == "Q8_0") return "最接近原始精度的一档量化，几乎看不出差别。";
    if (q == "Q6_K") return "画质和体积最平衡的一档，多数机器挑它。";
    if (q.rfind("Q5", 0) == 0) return "比 Q6_K 再小一点，差别要仔细看才看得出。";
    if (q == "Q4_K_M") return "常见的折中档。细节开始少，构图和动作还在。";
    if (q.rfind("Q4", 0) == 0) return "和 Q4_K_M 同一档，体积略有出入。";
    if (q.rfind("Q3", 0) == 0) return "明显退化，小卡兜底用。";
    if (q.rfind("Q2", 0) == 0) return "退化很重，只建议拿来验流程。";
    return "";
}

/// id 用的小写形式。`Q4_K_M` → `q4_k_m`，点换成横杠。
std::string lower_id(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '.') c = '-';
    }
    return s;
}

/// 「这一组先不下」。每组都有一个，理由见 catalog.hpp。
Option none_option(const std::string& label, const std::string& note,
                   std::vector<std::pair<std::string, json>> settings = {}) {
    Option o;
    o.id = kNoneOption;
    o.label = label;
    o.family_note = note;
    // rank 为负 = 永远不会被挑成默认值。挑默认值那个函数还会额外跳过它，
    // 见 recommend()——小卡上它的门槛是 0，不跳的话会成为唯一"够得上"的一项。
    o.rank = -1;
    o.settings = std::move(settings);
    return o;
}

// ---------------------------------------------------------------------------
// 编剧模型：Qwen3 四个尺寸 × 五档量化
// ---------------------------------------------------------------------------

struct LlmSpec {
    const char* size;  // "14B"
    const char* quant;
    std::uint64_t bytes;
};

// 全部来自 Qwen 官方的 GGUF 仓库，2026-09-10 用 ?blobs=true 核过。
constexpr LlmSpec kLlm[] = {
    {"32B", "Q8_0", 34817718912ULL},   {"32B", "Q6_K", 26883306112ULL},
    {"32B", "Q5_K_M", 23214831232ULL}, {"32B", "Q5_0", 22635493024ULL},
    {"32B", "Q4_K_M", 19762149024ULL},
    {"14B", "Q8_0", 15698533728ULL},   {"14B", "Q6_K", 12121937248ULL},
    {"14B", "Q5_K_M", 10514569568ULL}, {"14B", "Q5_0", 10263894400ULL},
    {"14B", "Q4_K_M", 9001752960ULL},
    {"8B", "Q8_0", 8709518112ULL},     {"8B", "Q6_K", 6725899040ULL},
    {"8B", "Q5_K_M", 5851112224ULL},   {"8B", "Q5_0", 5720761152ULL},
    {"8B", "Q4_K_M", 5027783488ULL},
    {"4B", "Q8_0", 4280404704ULL},     {"4B", "Q6_K", 3306260704ULL},
    {"4B", "Q5_K_M", 2889513184ULL},   {"4B", "Q5_0", 2823710976ULL},
    {"4B", "Q4_K_M", 2497280256ULL},
};

/// 参数量越大写得越好，同尺寸里量化越高越好。rank 按这个排。
///
/// **尺寸的权重压过量化**：14B 的 Q4_K_M 写得比 8B 的 Q8_0 好。
/// 反过来排的话，24 GB 的卡会被推荐一个 8B Q8_0，而那台机器装得下 14B。
int llm_rank(const std::string& size, const std::string& quant) {
    const int by_size = size == "32B" ? 400 : size == "14B" ? 300
                        : size == "8B" ? 200 : 100;
    const int by_quant = quant == "Q8_0" ? 5 : quant == "Q6_K" ? 4
                         : quant == "Q5_K_M" ? 3 : quant == "Q5_0" ? 2 : 1;
    return by_size + by_quant;
}

std::string llm_family_note(const std::string& family) {
    if (family == "Qwen3-32B") {
        return "写得最好的一档。代价是它和出图模型抢显存：40 GB 以下的卡"
               "每次写剧本都要把出图那套整个卸掉再装回来。";
    }
    if (family == "Qwen3-14B") {
        return "5090 上实测的一档：Q4_K_M 载进显存 15.4 GB，一次调用约 10 秒；"
               "出片时被驱逐，重载多花 4.6 秒。";
    }
    if (family == "Qwen3-8B") {
        return "小卡上的折中。剧本会短一些，人物关系容易写扁。";
    }
    return "什么卡都跑得动。只建议拿来验流程，写不出能用的剧本。";
}

// ---------------------------------------------------------------------------
// 出片：MiniMax-H3 / Wan 2.2 TI2V-5B
// ---------------------------------------------------------------------------

constexpr const char* kH3GgufRepo = "leejet/MiniMax-H3-GGUF";
constexpr const char* kH3ComfyRepo = "Comfy-Org/MiniMax-H3";
constexpr const char* kH3LoraRepo = "larryvrh/MiniMax-H3-Turbo-Lora";

constexpr const char* kH3FamilyNote =
    "画面和立体声一起生成，动作真实感这一档里最好，用户 2026-09-10 选定的"
    "就是它。两处代价：整套下载量最小也要 31 GB；授权禁止美国、欧盟、英国、"
    "韩国的创作者分发用它生成的视频。";

struct H3Spec {
    const char* id;
    const char* family;  // "MiniMax-H3 完整" / "MiniMax-H3 精简"
    const char* quant;
    const char* path;    // 相对下面那个仓库前缀
    const char* repo;
    std::uint64_t bytes;
    int rank;
};

// **只收 leejet 的 GGUF 和 Comfy 的纯 bf16。**
//
// int8_convrot 和 fp8_scaled 那几份没收：前者是个专门的格式，后者带 scale
// 张量——sd.cpp 的加载器里没有一行处理 scaled，会按普通 fp8 读，不报错，
// 只是出来的东西全是垃圾（Qwen2.5-VL 的 fp8_scaled 上已经栽过一次）。
// 清单里只放能确定读得对的：读不对这件事没有任何报错。
constexpr H3Spec kH3[] = {
    {"h3-full-bf16", "MiniMax-H3 完整", "bf16",
     "diffusion_models/minimax_h3_fl2va_bf16.safetensors", kH3ComfyRepo,
     66280487368ULL, 40},
    {"h3-full-q4_k_m", "MiniMax-H3 完整", "Q4_K_M",
     "minimax_h3_fl2va-Q4_K_M.gguf", kH3GgufRepo, 18779848448ULL, 30},
    {"h3-pruned-bf16", "MiniMax-H3 精简", "bf16",
     "diffusion_models/minimax_h3_fl2va_pruned_bf16.safetensors", kH3ComfyRepo,
     40225724176ULL, 25},
    {"h3-pruned-q4_k_m", "MiniMax-H3 精简", "Q4_K_M",
     "minimax_h3_fl2va_pruned-Q4_K_M.gguf", kH3GgufRepo, 11420663904ULL, 20},
};

constexpr const char* kWanFamilyNote =
    "最省显存的一条路，8 GB 的卡也跑得动，整套下载量只有 H3 的三分之一。"
    "代价是动作质量——用户看过它出的成片，原话是「图生视频还是太差了」。";

struct WanSpec {
    const char* quant;
    const char* file;
    std::uint64_t bytes;
    bool safetensors;  // fp16 那份在 Comfy 的仓库，量化版在 QuantStack
    int rank;
};

constexpr WanSpec kWan[] = {
    {"fp16", "wan2.2_ti2v_5B_fp16.safetensors", 9999658848ULL, true, 14},
    {"Q8_0", "Wan2.2-TI2V-5B-Q8_0.gguf", 5400179040ULL, false, 13},
    {"Q6_K", "Wan2.2-TI2V-5B-Q6_K.gguf", 4211683680ULL, false, 12},
    {"Q5_1", "Wan2.2-TI2V-5B-Q5_1.gguf", 3866636640ULL, false, 11},
    {"Q5_K_M", "Wan2.2-TI2V-5B-Q5_K_M.gguf", 3810603360ULL, false, 10},
    {"Q5_0", "Wan2.2-TI2V-5B-Q5_0.gguf", 3642503520ULL, false, 9},
    {"Q5_K_S", "Wan2.2-TI2V-5B-Q5_K_S.gguf", 3559928160ULL, false, 8},
    {"Q4_K_M", "Wan2.2-TI2V-5B-Q4_K_M.gguf", 3433116000ULL, false, 7},
    {"Q4_1", "Wan2.2-TI2V-5B-Q4_1.gguf", 3253219680ULL, false, 6},
    {"Q4_K_S", "Wan2.2-TI2V-5B-Q4_K_S.gguf", 3116380512ULL, false, 5},
    {"Q4_0", "Wan2.2-TI2V-5B-Q4_0.gguf", 3029086560ULL, false, 4},
    {"Q3_K_M", "Wan2.2-TI2V-5B-Q3_K_M.gguf", 2547790176ULL, false, 3},
    {"Q3_K_S", "Wan2.2-TI2V-5B-Q3_K_S.gguf", 2294755680ULL, false, 2},
    {"Q2_K", "Wan2.2-TI2V-5B-Q2_K.gguf", 1853862240ULL, false, 1},
};

/// Wan 的文本编码器（UMT5-XXL）按扩散模型那一档配。
///
/// **编码器的大小不影响显存门槛**：`weights_for` 里写着"文本编码器永远
/// 放内存"——它每镜只跑一次（实测 8 到 9 秒），而它常常是这一套里最大的
/// 一块。所以这里按"别让下载量失衡"来配：扩散模型都压到 3 GB 了，
/// 再配一个 11 GB 的编码器没道理。
FileSpec wan_encoder(double diff_gb) {
    if (diff_gb >= 9.0) {
        return {"umt5_xxl_fp16.safetensors",
                "Comfy-Org/Wan_2.1_ComfyUI_repackaged",
                "split_files/text_encoders/umt5_xxl_fp16.safetensors",
                11366399385ULL, "video_text_encoder",
                "文本编码器 UMT5-XXL（fp16 原版）。权重常驻内存"};
    }
    if (diff_gb >= 3.4) {
        return {"umt5-xxl-encoder-Q8_0.gguf", "city96/umt5-xxl-encoder-gguf",
                "umt5-xxl-encoder-Q8_0.gguf",
                6043068256ULL, "video_text_encoder",
                "文本编码器 UMT5-XXL（Q8_0）。权重常驻内存"};
    }
    return {"umt5-xxl-encoder-Q5_K_M.gguf", "city96/umt5-xxl-encoder-gguf",
            "umt5-xxl-encoder-Q5_K_M.gguf",
            4145878880ULL, "video_text_encoder",
            "文本编码器 UMT5-XXL（Q5_K_M）。权重常驻内存"};
}

// ---------------------------------------------------------------------------
// 出首帧：Qwen-Image
// ---------------------------------------------------------------------------

constexpr const char* kQwenImageFamilyNote =
    "首帧是跨镜头一致性的锚点，也是喂给出片那一步的起始图——糊了后面每一镜"
    "都糊，而且全程不报错。所以这一组值得往高了挑：5090 上实测 Q6_K 权重"
    "常驻显存是 35 秒一张，同一台机器上 fp8 那份放内存是 190 秒一张。";

struct ImageSpec {
    const char* quant;
    const char* file;
    std::uint64_t bytes;
    bool safetensors;  // fp8 那份在 Comfy 的仓库，其余在 city96
    int rank;
};

constexpr ImageSpec kImage[] = {
    {"BF16", "qwen-image-BF16.gguf", 40872114720ULL, false, 16},
    {"Q8_0", "qwen-image-Q8_0.gguf", 21761817120ULL, false, 15},
    {"fp8", "qwen_image_fp8_e4m3fn.safetensors", 20430635136ULL, true, 14},
    {"Q6_K", "qwen-image-Q6_K.gguf", 16824990240ULL, false, 13},
    {"Q5_1", "qwen-image-Q5_1.gguf", 15391717920ULL, false, 12},
    {"Q5_K_M", "qwen-image-Q5_K_M.gguf", 14934899232ULL, false, 11},
    {"Q5_0", "qwen-image-Q5_0.gguf", 14400813600ULL, false, 10},
    {"Q5_K_S", "qwen-image-Q5_K_S.gguf", 14117698080ULL, false, 9},
    {"Q4_K_M", "qwen-image-Q4_K_M.gguf", 13065746976ULL, false, 8},
    {"Q4_1", "qwen-image-Q4_1.gguf", 12843678240ULL, false, 7},
    {"Q4_K_S", "qwen-image-Q4_K_S.gguf", 12140608032ULL, false, 6},
    {"Q4_0", "qwen-image-Q4_0.gguf", 11852773920ULL, false, 5},
    {"Q3_K_M", "qwen-image-Q3_K_M.gguf", 9679567392ULL, false, 4},
    {"Q3_K_S", "qwen-image-Q3_K_S.gguf", 8952609312ULL, false, 3},
    {"Q2_K", "qwen-image-Q2_K.gguf", 7062518304ULL, false, 2},
};

/// Qwen-Image 的文本编码器（Qwen2.5-VL-7B）按扩散模型那一档配。
///
/// **别用 Comfy 那份 fp8_scaled**：那种格式带 scale 张量，sd.cpp 的加载器里
/// 没有一行处理 scaled，会按普通 fp8 读——不报错，只是文本条件全是垃圾。
/// 上游 docs/qwen_image.md 用的就是下面这份 Q8_0。
FileSpec image_encoder(double diff_gb) {
    if (diff_gb >= 20.0) {
        return {"qwen_2.5_vl_7b_bf16.safetensors", "Comfy-Org/Qwen-Image_ComfyUI",
                "split_files/text_encoders/qwen_2.5_vl_7b.safetensors",
                16584415576ULL, "image_text_encoder",
                "文本编码器 Qwen2.5-VL-7B（bf16 原版）。权重常驻内存"};
    }
    if (diff_gb >= 10.0) {
        return {"Qwen2.5-VL-7B-Instruct-Q8_0.gguf",
                "unsloth/Qwen2.5-VL-7B-Instruct-GGUF",
                "Qwen2.5-VL-7B-Instruct-Q8_0.gguf",
                8098524032ULL, "image_text_encoder",
                "文本编码器 Qwen2.5-VL-7B（Q8_0）。权重常驻内存"};
    }
    return {"Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf",
            "unsloth/Qwen2.5-VL-7B-Instruct-GGUF",
            "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf",
            4683072384ULL, "image_text_encoder",
            "文本编码器 Qwen2.5-VL-7B（Q4_K_M）。权重常驻内存"};
}

// ---------------------------------------------------------------------------
// 配音：Qwen3-TTS
// ---------------------------------------------------------------------------

constexpr const char* kTtsRepo = "ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF";
constexpr const char* kTtsFamilyNote =
    "进程内跑，不用另起服务。这一组是四组里最小的，而且几乎不占显存"
    "——1.7B 的模型，什么卡都放得下。";

struct TtsSpec {
    const char* quant;
    const char* backbone;
    std::uint64_t backbone_bytes;
    const char* mmproj;
    std::uint64_t mmproj_bytes;
    int rank;
};

constexpr TtsSpec kTts[] = {
    {"bf16", "Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf", 3472593760ULL,
     "mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf", 669081472ULL, 30},
    {"Q8_0", "Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf", 1847874400ULL,
     "mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf", 446422912ULL, 20},
    // Q4_K_M 那一档没有配套的 mmproj，配 Q8_0 那份——解码器本身很小，
    // 省那 200 MB 没有意义，而**拿错了 mtmd 会报"这份 mmproj 不支持音频生成"**。
    {"Q4_K_M", "Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf", 1035965280ULL,
     "mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf", 446422912ULL, 10},
};

// ---------------------------------------------------------------------------

std::vector<Group> build() {
    std::vector<Group> gs;

    // ---------------- 编剧 ----------------
    {
        Group g;
        g.key = "llm";
        g.title = "编剧模型";
        g.purpose = "写剧本大纲、拆分镜、提角色。整条流水线的第一步。";
        g.required = true;
        g.owned_roles = {"llm"};

        for (const auto& spec : kLlm) {
            const std::string size = spec.size;
            const std::string quant = spec.quant;
            const std::string file = "Qwen3-" + size + "-" + quant + ".gguf";
            Option o;
            o.id = lower_id("qwen3-" + size + "-" + quant);
            o.family = "Qwen3-" + size;
            o.label = "Qwen3-" + size + " · " + quant;
            o.quant = quant;
            o.family_note = llm_family_note(o.family);
            o.note = quant_note(quant);
            // **和出图出片不一样：这条路没有"权重放内存"那一档。**
            // llama.cpp 是整个载进显存的，装不下就是载不进去。
            o.min_vram_gb = llm_vram(gb(spec.bytes));
            o.rank = llm_rank(size, quant);
            o.files.push_back({"llm/" + file, "Qwen/Qwen3-" + size + "-GGUF", file,
                               spec.bytes, "llm", "编剧模型本体"});
            // 下了模型就该用进程内那条路，否则下完还得自己去设置页切一下，
            // 而不切的表现是「写剧本」按钮报连不上 127.0.0.1:11434。
            o.settings.push_back({"llm.backend", "local"});
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "不下载 · 用外接大模型服务",
            "剧本交给别的机器或者云端（Ollama、vLLM、DeepSeek 之类）。"
            "选它之后去设置页填地址和密钥。",
            {{"llm.backend", "remote"}}));

        gs.push_back(std::move(g));
    }

    // ---------------- 出片 ----------------
    {
        Group g;
        g.key = "video";
        g.title = "出片模型（图生视频）";
        g.purpose = "把每一镜的首帧变成一段视频。这一组最大，也最花时间。";
        g.required = true;
        // **八个键一起管。** 换家族时没用到的必须清空，尤其是 video_lora——
        // 它的默认值指着 H3 的 Turbo LoRA，留着的话会被挂到 Wan 上。
        g.owned_roles = {"video",     "video_high_noise", "video_vae",
                         "video_text_encoder", "video_llm", "video_llm_vision",
                         "video_audio_vae",    "video_lora"};

        for (const auto& spec : kH3) {
            Option o;
            o.id = spec.id;
            o.family = spec.family;
            o.label = std::string(spec.family) + " · " + spec.quant;
            o.quant = spec.quant;
            o.family_note = kH3FamilyNote;
            o.note = quant_note(spec.quant);
            o.min_vram_gb = resident_vram(false, gb(spec.bytes));
            o.rank = spec.rank;

            const std::string path = spec.path;
            const auto slash = path.rfind('/');
            const std::string file =
                slash == std::string::npos ? path : path.substr(slash + 1);
            o.files.push_back({file, spec.repo, path, spec.bytes, "video",
                               "扩散模型。画面和立体声一起生成"});
            // 编码器按扩散模型那一档配。它常驻内存，不影响显存门槛。
            const bool big = gb(spec.bytes) >= 15.0;
            const std::string enc = big ? "qwen3vl_32b_minimax_h3-Q4_K_M.gguf"
                                        : "qwen3vl_32b_minimax_h3-Q2_K_M.gguf";
            o.files.push_back(
                {enc, kH3GgufRepo, enc,
                 big ? 18218065024ULL : 13102161024ULL, "video_llm",
                 "文本编码器（裁过的 Qwen3-VL-32B）。每镜只跑一次，权重常驻内存"});
            o.files.push_back({"minimax_h3_video_vae_fp16.safetensors",
                               kH3ComfyRepo,
                               "vae/minimax_h3_video_vae_fp16.safetensors",
                               5207808496ULL, "video_vae", "视频 VAE"});
            o.files.push_back({"minimax_h3_audio_vae_fp32.safetensors",
                               kH3ComfyRepo,
                               "vae/minimax_h3_audio_vae_fp32.safetensors",
                               605254808ULL, "video_audio_vae",
                               "音频 VAE。不下这份片子出来是没有音轨的"});
            o.files.push_back(
                {"loras/minimax_h3_turbo_v4_step600_ema.safetensors", kH3LoraRepo,
                 "minimax_h3_turbo_v4_step600_ema.safetensors",
                 779849816ULL, "video_lora",
                 "Turbo 蒸馏。采样从 28 步压到 6 步，实测一镜 242 秒降到 124 秒"});

            // H3 的三个旋钮和 Wan 完全不同，**填错了三处都不报错**：
            // 编码器走 video_llm 不是 video_text_encoder（上面已经这么填了）、
            // cfg 是 1.0 不是 6.0、随机数发生器要 cpu。
            // 错了的表现是出来的片和提示词没关系，人会先去怀疑提示词。
            o.settings.push_back({"models.video_rng", "cpu"});
            o.settings.push_back({"models.video_cfg", 1.0});
            o.settings.push_back({"models.video_lora_strength", 1.0});
            o.settings.push_back({"models.video_lora_tiers", "both"});
            // **步数交给引擎自己算，这里一定要写 0（＝没填）。**
            //
            // 挂了 Turbo 就该跑 6 步，但那件事 `config::effective_spec` 已经
            // 做了——它看 video_lora 那个文件在不在，在就把出片步数压到 6。
            // 这里再写死一个 6 的后果是**首帧跟着糊掉**：`[tiers].final_steps`
            // 会落进档位表（见 Runtime::profile），而首帧的步数取的正是档位表
            // 那个数（effective_spec 里 frame_steps = table_final_steps）。
            // Turbo 那个 LoRA 只挂在视频模型上，出图那一步没有它，6 步就是
            // 裸跑 6 步。首帧是跨镜头一致性的锚点，糊了后面每一镜都糊。
            //
            // 实测踩到过（2026-09-10，就在这一版上）：设置页从
            // 「出片 6 Turbo · 首帧 30」变成了「出片 6 Turbo · 首帧 6」。
            o.settings.push_back({"tiers.final_steps", 0});
            g.options.push_back(std::move(o));
        }

        for (const auto& spec : kWan) {
            Option o;
            o.id = lower_id(std::string("wan22-ti2v-5b-") + spec.quant);
            o.family = "Wan 2.2 TI2V-5B";
            o.label = std::string("Wan 2.2 TI2V-5B · ") + spec.quant;
            o.quant = spec.quant;
            o.family_note = kWanFamilyNote;
            o.note = quant_note(spec.quant);
            o.min_vram_gb = resident_vram(false, gb(spec.bytes));
            o.rank = spec.rank;

            o.files.push_back(
                {spec.file,
                 spec.safetensors ? "Comfy-Org/Wan_2.2_ComfyUI_Repackaged"
                                  : "QuantStack/Wan2.2-TI2V-5B-GGUF",
                 spec.safetensors
                     ? "split_files/diffusion_models/" + std::string(spec.file)
                     : std::string(spec.file),
                 spec.bytes, "video", "扩散模型"});
            // **VAE 必须是 2.2 那份，不是 2.1 的。** 只有 TI2V-5B 用这一份，
            // 拿错了出来的是花屏。镜像上的文件名是小写的，落盘按上游习惯存。
            o.files.push_back(
                {"Wan2.2_VAE.safetensors", "Comfy-Org/Wan_2.2_ComfyUI_Repackaged",
                 "split_files/vae/wan2.2_vae.safetensors",
                 1409400960ULL, "video_vae", "VAE。是 2.2 那份，2.1 的不通用"});
            o.files.push_back(wan_encoder(gb(spec.bytes)));

            // 上游 docs/wan.md 给 TI2V-5B 的命令行就是这两个数。
            o.settings.push_back({"models.video_cfg", 6.0});
            o.settings.push_back({"models.video_flow_shift", 3.0});
            o.settings.push_back({"models.video_rng", "cuda"});
            // 同 H3 那条：0 = 没填，按显存推的档位表走。写死 30 的话换台卡
            // （8 GB 的机器档位表推的是 25 步）就不对了，而且一样会把首帧的
            // 步数一起钉死。
            o.settings.push_back({"tiers.final_steps", 0});
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "不下载 · 之后再说",
            "跳过这一组的话出不了片，只能走到分镜为止。"));

        gs.push_back(std::move(g));
    }

    // ---------------- 出首帧 ----------------
    {
        Group g;
        g.key = "image";
        g.title = "首帧模型（文生图）";
        g.purpose =
            "每一镜先出一张首帧，再由它生成视频。首帧是跨镜头一致性的锚点，"
            "糊了后面每一镜都糊。";
        g.required = true;
        g.owned_roles = {"image", "image_vae", "image_text_encoder",
                         "image_text_encoder_vision"};

        const FileSpec vae{
            "qwen_image_vae.safetensors", "Comfy-Org/Qwen-Image_ComfyUI",
            "split_files/vae/qwen_image_vae.safetensors",
            253806246ULL, "image_vae",
            "VAE。不能复用视频那份——Wan 的 VAE 和 Qwen-Image 的不是一回事，"
            "喂错了不报错，只是出一张和提示词没关系的图"};

        for (const auto& spec : kImage) {
            Option o;
            o.id = lower_id(std::string("qwen-image-") + spec.quant);
            o.family = "Qwen-Image";
            o.label = std::string("Qwen-Image · ") + spec.quant;
            o.quant = spec.quant;
            o.family_note = kQwenImageFamilyNote;
            o.note = quant_note(spec.quant);
            o.min_vram_gb = resident_vram(true, gb(spec.bytes));
            o.rank = spec.rank;

            o.files.push_back(
                {spec.file,
                 spec.safetensors ? "Comfy-Org/Qwen-Image_ComfyUI"
                                  : "city96/Qwen-Image-gguf",
                 spec.safetensors
                     ? "split_files/diffusion_models/" + std::string(spec.file)
                     : std::string(spec.file),
                 spec.bytes, "image", "扩散模型"});
            o.files.push_back(vae);
            o.files.push_back(image_encoder(gb(spec.bytes)));
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "不下载 · 之后再说",
            "跳过这一组的话出不了首帧，出片那一步也就没有起始图。"));

        gs.push_back(std::move(g));
    }

    // ---------------- 配音 ----------------
    {
        Group g;
        g.key = "tts";
        g.title = "配音模型";
        g.purpose = "把台词念出来。装配成片时按台词时长对齐镜头。";
        // **非必需**：没有它整集是静音的，但剧本、分镜、画面这条路照样走得通。
        // 卡在这儿不让人进首页，等于因为一个 4 GB 的模型把整个程序锁住。
        g.required = false;
        g.owned_roles = {"tts", "tts_decoder"};

        for (const auto& spec : kTts) {
            Option o;
            o.id = lower_id(std::string("qwen3-tts-") + spec.quant);
            o.family = "Qwen3-TTS 12Hz 1.7B";
            o.label = std::string("Qwen3-TTS 12Hz 1.7B · ") + spec.quant;
            o.quant = spec.quant;
            o.family_note = kTtsFamilyNote;
            o.note = quant_note(spec.quant);
            o.min_vram_gb = llm_vram(gb(spec.backbone_bytes));
            o.rank = spec.rank;
            o.files.push_back({spec.backbone, kTtsRepo, spec.backbone,
                               spec.backbone_bytes, "tts", "骨干（talker）"});
            o.files.push_back({spec.mmproj, kTtsRepo, spec.mmproj,
                               spec.mmproj_bytes, "tts_decoder",
                               "解码器，把码本还原成波形"});
            o.settings.push_back({"tts.backend", "local"});
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "不下载 · 整集先没有人声",
            "剧本、分镜、画面照常出，只是没有配音。之后随时可以回到设置页补下。"));

        gs.push_back(std::move(g));
    }

    return gs;
}

}  // namespace

std::uint64_t Option::total_bytes() const {
    std::uint64_t n = 0;
    for (const auto& f : files) n += f.bytes;
    return n;
}

const Option* Group::find(const std::string& option_id) const {
    for (const auto& o : options) {
        if (o.id == option_id) return &o;
    }
    return nullptr;
}

const std::vector<Group>& catalog() {
    static const std::vector<Group> gs = build();
    return gs;
}

std::map<std::string, std::string> recommend(double vram_gb) {
    std::map<std::string, std::string> out;
    for (const auto& g : catalog()) {
        // **编剧模型的预算只给六成。**
        //
        // 它和出图出片模型共用这张卡。三者不同时跑（调度器会把上一个卸掉），
        // 所以"装不装得下"看的是单个；但**每次来回都要重载一遍**，而重载
        // 的代价随模型大小涨——5090 上实测 14B Q4_K_M 重载一次 4.6 秒，
        // 32B 只会更久，而写剧本和出图在一集里要来回切几十次。
        //
        // 挑推荐值时因此往小了留一档。**这只影响默认值**：用户想用 32B
        // 照样点得动，那一档的显存门槛也照实显示。
        const double budget = g.key == "llm" ? vram_gb * 0.6 : vram_gb;
        const Option* best = nullptr;
        for (const auto& o : g.options) {
            // **「不下载」不参与挑选。** 它的门槛是 0，小卡上会是唯一
            // 够得上的一项——于是推荐出来的默认值变成"什么都不装"，
            // 而那正是这一页要解决的状态。
            if (o.id == kNoneOption) continue;
            if (o.min_vram_gb > budget) continue;
            if (best == nullptr || o.rank > best->rank) best = &o;
        }
        if (best == nullptr) {
            // 一个都够不上（比如探不到显卡、按 12 GB 估的机器碰上一组
            // 全是大模型）。**不能留空**——界面上"没有默认值"等于让用户
            // 自己去猜。挑门槛最低的那个，代价写在 note 里。
            for (const auto& o : g.options) {
                if (o.id == kNoneOption) continue;
                if (best == nullptr || o.min_vram_gb < best->min_vram_gb) best = &o;
            }
        }
        if (best != nullptr) out[g.key] = best->id;
    }
    return out;
}

json config_patch(const std::map<std::string, std::string>& selections) {
    json patch = json::object();

    // 键形如 "models.video_rng"，拆成 {"models": {"video_rng": …}}。
    // 不带点的当顶层标量（vram_gb_override 那种就不在任何小节里）。
    const auto put = [&patch](const std::string& dotted, const json& value) {
        const auto dot = dotted.find('.');
        if (dot == std::string::npos) {
            patch[dotted] = value;
            return;
        }
        const std::string section = dotted.substr(0, dot);
        const std::string key = dotted.substr(dot + 1);
        if (!patch.contains(section) || !patch[section].is_object()) {
            patch[section] = json::object();
        }
        patch[section][key] = value;
    };

    for (const auto& g : catalog()) {
        const auto it = selections.find(g.key);
        if (it == selections.end()) continue;
        const Option* opt = g.find(it->second);
        // 认不出的 id 就跳过这一组。老界面提交一个已经删掉的选项时，
        // 少写一组比整个请求失败好。
        if (opt == nullptr) continue;

        for (const auto& [key, value] : opt->settings) put(key, value);

        if (opt->id == kNoneOption) continue;  // 不下载就不动 owned_roles

        // **每个键都写。** 没用到的写空串，否则上一次选的模型会留在配置里
        // 被当成这一次的一部分——video_lora 就是这么栽的。
        std::map<std::string, std::string> filled;
        for (const auto& f : opt->files) {
            if (!f.role.empty()) filled[f.role] = f.name;
        }
        for (const auto& role : g.owned_roles) {
            const auto hit = filled.find(role);
            put("models." + role, hit == filled.end() ? std::string() : hit->second);
        }
    }

    return patch;
}

}  // namespace changji::setup
