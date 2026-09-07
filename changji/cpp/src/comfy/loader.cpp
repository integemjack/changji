#include "comfy/loader.hpp"

#include <filesystem>
#include <utility>
#include <vector>

#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::comfy {

#include "comfy/bundled_workflows.inc.hpp"

std::optional<std::string> bundled_workflow(const std::string& name) {
    for (const auto& [n, text] : bundled_workflows()) {
        if (name == n) return std::string(text);
    }
    return std::nullopt;
}

std::optional<ApiWorkflow> load_workflow(Client& client,
                                         const models::ProjectStore& store,
                                         const std::string& name,
                                         bool required) {
    // 项目里的优先。同名文件覆盖内置的那份，不同的剧用不同的模型靠这个。
    const fs::path candidate =
        store.root() / paths::from_utf8("workflows") /
        paths::from_utf8(name + ".json");

    OrderedJson raw;
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) {
        raw = load_ui_workflow(candidate);
    } else if (const auto text = bundled_workflow(name)) {
        raw = OrderedJson::parse(*text, nullptr, false);
        if (raw.is_discarded()) {
            // 内置的那份是生成出来的，坏了说明生成器或者源文件出了问题，
            // 不是用户能修的——所以话要说给开发看。
            throw WorkflowError("内置的 " + name +
                                " 工作流不是合法 JSON，重跑 gen_workflows.py");
        }
    } else {
        if (!required) return std::nullopt;
        throw WorkflowError("找不到 " + name +
                            " 工作流。请把 ComfyUI 里导出的工作流保存到"
                            "项目的 workflows/" + name + ".json");
    }

    // 界面版才需要转。已经是接口版的直接包一层——**这条很要紧**：
    // 转换要拉服务端的 /object_info，而接口版根本不需要服务端在线。
    if (raw.contains("nodes")) {
        return client.converter().convert(raw);
    }
    return ApiWorkflow(std::move(raw));
}

std::map<std::string, std::optional<ApiWorkflow>> load_all(
    Client& client, const models::ProjectStore& store) {
    std::map<std::string, std::optional<ApiWorkflow>> out;
    out.emplace("video", load_workflow(client, store, "video", true));
    out.emplace("image", load_workflow(client, store, "image", false));
    out.emplace("tts", load_workflow(client, store, "tts", false));
    return out;
}

}  // namespace changji::comfy
