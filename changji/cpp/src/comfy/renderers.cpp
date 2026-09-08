#include "comfy/renderers.hpp"

#include <array>
#include <filesystem>

#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::comfy {

namespace {

/// 连线型输入长这样：["源节点id", 输出槽]。
std::string link_source(const OrderedJson& v) {
    if (!v.is_array() || v.empty()) return {};
    if (v[0].is_string()) return v[0].get<std::string>();
    if (v[0].is_number_integer()) return std::to_string(v[0].get<long long>());
    return {};
}

}  // namespace

std::pair<std::string, std::string> text_node_ids(const ApiWorkflow& w) {
    const std::string ks = w.one_by_class("KSampler");
    const OrderedJson pos = w.get_input(ks, "positive");
    const OrderedJson neg = w.get_input(ks, "negative");
    const std::string p = link_source(pos);
    const std::string n = link_source(neg);
    if (p.empty() || n.empty()) {
        // 按 id 大小猜是错的：那只是画布上的创建顺序。猜错的后果是
        // 正负提示词对调，画面里出现的全是负面词里写的东西，而且不报错。
        throw WorkflowError("工作流的 KSampler 没有接正负提示词，无法确定改哪个节点");
    }
    return {p, n};
}

bool set_size(ApiWorkflow& w, int width, int height) {
    // 顺序有讲究：先试视频那个（带 length），再试通用的空潜变量节点。
    // 反过来的话，一个同时有两种节点的工作流会去改那个不影响输出的。
    static const std::array<const char*, 4> kSizeNodes = {
        "Wan22ImageToVideoLatent", "EmptySD3LatentImage", "EmptyLatentImage",
        "EmptyHunyuanLatentVideo",
    };
    for (const char* cls : kSizeNodes) {
        try {
            w.set_by_class(cls, {{"width", width}, {"height", height}});
            return true;
        } catch (const WorkflowError&) {
            continue;
        }
    }
    return false;
}

stages::FrameRenderer image_frame_renderer(std::shared_ptr<Client> client,
                                           ApiWorkflow workflow,
                                           models::ProjectPaths paths) {
    // 按值捕获工作流：每一镜都要改一份副本，共用一份的话上一镜的
    // 提示词会留在里面，而"忘了改"和"改了没生效"在产出上是一样的。
    return [client, workflow, paths](
               const models::Shot& shot, const stages::PromptBundle& prompts,
               const models::TierSpec& spec, const fs::path& dest,
               pipeline::CancelToken& tok, const infer::StepCallback& on_step) {
        ApiWorkflow wf(workflow.to_json());   // 深拷贝

        set_size(wf, spec.width, spec.height);
        wf.set_by_class("KSampler",
                        {{"seed", stages::frame_seed(shot.shot_id, shot.attempts)}});

        const auto [pos_id, neg_id] = text_node_ids(wf);
        wf.set_input(pos_id, "text", prompts.positive);
        wf.set_input(neg_id, "text", prompts.negative);

        // 参考图要先传上去：ComfyUI 可能在另一台机器上，本地路径它读不到。
        // 有几个 LoadImage 就填几张，多出来的参考图丢掉——
        // 工作流能吃几张是工作流的事，这一层不该去改它的结构。
        std::vector<std::string> uploaded;
        for (const auto& rel : prompts.reference_images) {
            const fs::path abs = paths.abs(rel);
            std::error_code ec;
            if (fs::is_regular_file(abs, ec)) {
                uploaded.push_back(client->upload_image(abs));
            }
        }
        const auto loaders = wf.find_by_class("LoadImage");
        for (std::size_t i = 0; i < loaders.size() && i < uploaded.size(); ++i) {
            wf.set_input(loaders[i], "image", uploaded[i]);
        }

        try {
            wf.set_by_class("SaveImage",
                            {{"filename_prefix", "changji/frame_" + shot.shot_id}});
        } catch (const WorkflowError&) {
            // 有些工作流用别的保存节点，用默认前缀也能取到产出
        }

        JobResult result;
        try {
            result = client->run(wf, [&](const JobProgress& p) {
                if (p.total > 0) on_step(p.step, p.total, 0.0, false);
            }, tok);
        } catch (const PromptValidationError& e) {
            // 原样透传的话用户看到的是一坨嵌套 JSON。
            throw ComfyError("镜头 " + shot.shot_id +
                                     " 的图像工作流被拒绝：\n" + e.human_summary());
        }

        const auto ref = result.first_file();
        if (!ref.has_value()) {
            throw ComfyError("镜头 " + shot.shot_id + " 首帧生成完成但没有产出");
        }
        client->download(*ref, dest);
    };
}

stages::VideoRenderer video_renderer(std::shared_ptr<Client> client,
                                     ApiWorkflow workflow) {
    return [client, workflow](
               const models::Shot& shot, const stages::RenderPlan& plan,
               const std::optional<fs::path>& start_image, const fs::path& dest,
               pipeline::CancelToken& tok, const infer::StepCallback& on_step) {
        ApiWorkflow wf(workflow.to_json());

        // 尺寸、帧数、步数、种子。
        wf.set_by_class("Wan22ImageToVideoLatent",
                        {{"width", plan.spec.width},
                         {"height", plan.spec.height},
                         {"length", plan.frames}});
        wf.set_by_class("KSampler",
                        {{"steps", plan.spec.steps},
                         {"seed", stages::render_seed(shot.shot_id, shot.attempts)}});

        const auto [pos_id, neg_id] = text_node_ids(wf);
        wf.set_input(pos_id, "text", stages::video_positive(plan));
        wf.set_input(neg_id, "text", plan.prompts.negative);

        if (start_image.has_value()) {
            wf.set_by_class("LoadImage",
                            {{"image", client->upload_image(*start_image)}});
        }

        const std::string tier =
            plan.tier == models::Tier::FINAL ? "final" : "draft";
        wf.set_by_class("SaveVideo",
                        {{"filename_prefix", "changji/" + shot.shot_id + "_" + tier}});

        JobResult result;
        try {
            result = client->run(wf, [&](const JobProgress& p) {
                if (p.total > 0) on_step(p.step, p.total, 0.0, false);
            }, tok);
        } catch (const PromptValidationError& e) {
            throw ComfyError("镜头 " + shot.shot_id +
                                 " 的工作流被服务端拒绝：\n" + e.human_summary());
        }

        const auto ref = result.first_file();
        if (!ref.has_value()) {
            throw ComfyError("镜头 " + shot.shot_id + " 生成完成但没有产出文件");
        }
        client->download(*ref, dest);
    };
}

}  // namespace changji::comfy
