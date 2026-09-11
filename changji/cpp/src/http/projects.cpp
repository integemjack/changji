#include "http/projects.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

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

std::string opt_str(const json& body, const char* key, const std::string& def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

/// target 是不是**严格**在 root 里面。
///
/// 严格：等于 root 本身不算。等于的话这个接口就能删掉整个项目库，
/// 那是"一次误点删掉所有项目"，比删错一个项目严重得多。
///
/// 两边都先规范化。不规范化的话 `项目库/a/../../别处` 这种路径
/// 字面上看着在里面，实际指向外面。
bool strictly_inside(const fs::path& target, const fs::path& root) {
    std::error_code ec;
    const fs::path t = fs::weakly_canonical(target, ec);
    if (ec) return false;
    const fs::path r = fs::weakly_canonical(root, ec);
    if (ec) return false;
    if (t == r) return false;
    const fs::path rel = t.lexically_relative(r);
    if (rel.empty()) return false;
    // lexically_relative 会产出 ".." 开头的路径表示"在外面"
    const std::string first = paths::to_utf8(*rel.begin());
    return first != "..";
}

/// 把请求里那个 path 解析成真正的目录。
///
/// **只填名字（不带路径分隔符）就落在项目库根目录下。** 用户不知道项目库
/// 挂在哪（容器里、用户数据目录里），让他猜绝对路径没道理。
///
/// 这条规则以前只写在新建那一支里，删除那边直接把原样的字符串交给
/// weakly_canonical——而那是按**进程的工作目录**解析的。于是同一个
/// "我的项目" 传给两个接口指的是两个地方：新建成功了，拿同样的字符串去
/// 删就是 403「只能删项目库里面的」，用户看不出自己哪里错了。
/// 2026-09-11 实测撞到。抽成一处，两边不会再分叉。
fs::path resolve_project_path(const std::string& raw,
                              const config::Settings& settings) {
    fs::path path = paths::expand_user(raw);
    if (!path.is_absolute() && std::distance(path.begin(), path.end()) == 1) {
        return settings.workspace_path() / paths::from_utf8(raw);
    }
    return path;
}

}  // namespace

ApiResult post_new_project(const json& body, const config::Settings& settings) {
    const std::string style = opt_str(body, "style_line", "realistic");
    if (style != "realistic" && style != "anime") {
        throw ApiError(400, "风格线只能是 realistic 或 anime");
    }
    const StyleLine line =
        style == "anime" ? StyleLine::ANIME : StyleLine::REALISTIC;

    const std::string raw = text::strip_ws(need_str(body, "path"));
    if (raw.empty()) throw ApiError(400, "得给项目起个名字");

    // 只填名字就落在项目库根目录下。见 resolve_project_path。
    const fs::path path = resolve_project_path(raw, settings);

    const std::string name = paths::to_utf8(path.filename());
    const std::string title = opt_str(body, "title", "");
    try {
        ProjectStore store = ProjectStore::create(
            path, text::project_slug(name), title.empty() ? name : title, line);
        return {200, {{"root", paths::to_utf8(store.root())}}};
    } catch (const fs::filesystem_error& e) {
        throw ApiError(400, std::string("创建目录失败：") + e.what());
    } catch (const std::exception& e) {
        // ProjectStore::create 在目录已存在时抛这个。409 不是 400——
        // 前端据此提示"换个名字"，而 400 会被当成参数错。
        const std::string msg = e.what();
        if (msg.find("已经") != std::string::npos ||
            msg.find("存在") != std::string::npos) {
            throw ApiError(409, msg);
        }
        throw ApiError(400, msg);
    }
}

ApiResult post_delete_project(const json& body,
                              const config::Settings& settings) {
    const std::string raw = need_str(body, "path");
    const std::string confirm = need_str(body, "confirm_name");

    // 跑到一半删项目，工作线程下一次写盘会写到一个不存在的目录上，
    // 报的错和"删项目"八竿子打不着。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, "正在跑，删项目会把跑到一半的东西弄坏");
    }

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(settings.workspace_path(), ec);
    if (ec) throw ApiError(400, "项目库路径不对：" + ec.message());

    // **和新建用同一条解析规则。** 以前这里是 expand_user 之后直接交给
    // weakly_canonical，相对路径按进程的工作目录算——新建时填 "我的项目"
    // 建在项目库里，删除时填同一个字符串却指到别处，报 403。
    const fs::path target = resolve_project_path(raw, settings);

    // 闸一：只让删项目库里面的。别的路径可能是用户自己放在别处的项目，
    // 也可能是手滑填的系统目录，不该由这个接口负责。
    if (!strictly_inside(target, root)) {
        throw ApiError(403, "只能删项目库 " + paths::to_utf8(root) +
                                " 里面的项目。别处的请自己删");
    }

    // 闸二：必须确实是一个项目。指到一个普通目录上的话，
    // 删掉的可能是用户放素材的地方。
    const ProjectStore store(target);
    if (!store.exists()) throw ApiError(404, "这个目录不是一个项目");

    // 闸三：名字一字不差。防的是"选错了一行然后顺手点了确认"。
    const std::string name = paths::to_utf8(
        fs::weakly_canonical(target, ec).filename());
    if (confirm != name) {
        throw ApiError(400, "确认名字对不上，要一字不差地填 " + name);
    }

    const fs::path resolved = fs::weakly_canonical(target, ec);
    if (ec) throw ApiError(400, "路径不对：" + ec.message());
    fs::remove_all(resolved, ec);
    if (ec) throw ApiError(500, "删不掉：" + ec.message());
    return {200, {{"deleted", paths::to_utf8(resolved)}}};
}

ApiResult post_project_premise(const json& body) {
    const std::string path = need_str(body, "project");
    if (path.empty()) throw ApiError(400, "没有指定项目目录");
    const std::string premise = need_str(body, "premise");

    ProjectStore store(paths::from_utf8(path));
    Project project;
    try {
        project = store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
    project.premise = text::truncate_utf8(text::strip_ws(premise), 2000);
    store.save_project(project);
    return {200, {{"premise", project.premise}}};
}

}  // namespace changji::http
