#include "comfy/workflow.hpp"

#include <fstream>
#include <set>
#include <sstream>

#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::comfy {

namespace {

/// 控件型输入的标量类型。除此之外的全大写类型名都是连线型输入
/// （MODEL、CLIP、VAE、IMAGE、LATENT、CONDITIONING、AUDIO、VIDEO 等）。
const std::set<std::string>& scalar_widget_types() {
    static const std::set<std::string> s = {"INT", "FLOAT", "STRING", "BOOLEAN"};
    return s;
}

/// 界面在 seed 后面插的那个伪控件的取值。
///
/// 判断靠**值**而不是靠名字：伪控件在 widgets_values 里只有值没有名字，
/// 而 /object_info 里根本没有它——名字无从查起。
bool is_seed_control_value(const OrderedJson& v) {
    if (!v.is_string()) return false;
    const std::string s = v.get<std::string>();
    return s == "fixed" || s == "increment" || s == "decrement" ||
           s == "randomize";
}

std::string node_id_of(const OrderedJson& node) {
    const auto it = node.find("id");
    if (it == node.end()) throw WorkflowError("工作流里有个节点没有 id");
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    throw WorkflowError("节点 id 既不是字符串也不是整数：" + it->dump());
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

}  // namespace

bool is_widget_type(const OrderedJson& type_def) {
    // 下拉框：选项直接列在这里
    if (type_def.is_array()) return true;
    if (!type_def.is_string()) return false;
    const std::string t = type_def.get<std::string>();
    if (scalar_widget_types().count(t)) return true;
    // COMBO、COMFY_DYNAMICCOMBO_V3 之类的下拉框变体
    return t.find("COMBO") != std::string::npos;
}

// ---- ApiWorkflow ----

std::vector<std::string> ApiWorkflow::find_by_class(
    const std::string& class_type) const {
    std::vector<std::string> out;
    if (!prompt_.is_object()) return out;
    for (const auto& kv : prompt_.items()) {
        const auto it = kv.value().find("class_type");
        if (it != kv.value().end() && it->is_string() &&
            it->get<std::string>() == class_type) {
            out.push_back(kv.key());
        }
    }
    return out;
}

std::string ApiWorkflow::one_by_class(const std::string& class_type) const {
    const auto found = find_by_class(class_type);
    if (found.empty()) {
        throw WorkflowError("工作流里没有 " + class_type + " 节点");
    }
    if (found.size() > 1) {
        throw WorkflowError("工作流里有 " + std::to_string(found.size()) + " 个 " +
                            class_type + " 节点，无法确定改哪个。"
                            "请用节点 id 指定：" + join(found, "、"));
    }
    return found[0];
}

void ApiWorkflow::set_input(const std::string& node_id, const std::string& key,
                            OrderedJson value) {
    const auto it = prompt_.find(node_id);
    if (it == prompt_.end()) {
        throw WorkflowError("节点 " + node_id + " 不存在");
    }
    if (!it->contains("inputs") || !(*it)["inputs"].is_object()) {
        (*it)["inputs"] = OrderedJson::object();
    }
    (*it)["inputs"][key] = std::move(value);
}

std::string ApiWorkflow::set_by_class(
    const std::string& class_type,
    const std::vector<std::pair<std::string, OrderedJson>>& kv) {
    const std::string nid = one_by_class(class_type);
    for (const auto& [key, value] : kv) set_input(nid, key, value);
    return nid;
}

OrderedJson ApiWorkflow::get_input(const std::string& node_id,
                                   const std::string& key) const {
    const auto node = prompt_.find(node_id);
    if (node == prompt_.end()) return nullptr;
    const auto inputs = node->find("inputs");
    if (inputs == node->end() || !inputs->is_object()) return nullptr;
    const auto v = inputs->find(key);
    if (v == inputs->end()) return nullptr;
    return *v;
}

// ---- WorkflowConverter ----

const std::vector<std::string>& WorkflowConverter::widget_names(
    const std::string& class_type) {
    const auto cached = widget_names_.find(class_type);
    if (cached != widget_names_.end()) return cached->second;

    const auto info = object_info_.find(class_type);
    if (info == object_info_.end()) {
        throw WorkflowError("服务端不认识节点类型 " + class_type +
                            "。通常是缺少对应的自定义节点包");
    }

    std::vector<std::string> names;
    const auto input_spec = info->find("input");
    if (input_spec != info->end() && input_spec->is_object()) {
        // required 在前 optional 在后，和界面上的顺序一致。
        for (const char* section : {"required", "optional"}) {
            const auto sec = input_spec->find(section);
            if (sec == input_spec->end() || !sec->is_object()) continue;
            for (const auto& kv : sec->items()) {
                const OrderedJson& spec = kv.value();
                if (!spec.is_array() || spec.empty()) continue;
                if (is_widget_type(spec[0])) names.push_back(kv.key());
            }
        }
    }
    return widget_names_.emplace(class_type, std::move(names)).first->second;
}

OrderedJson WorkflowConverter::zip_widgets(const std::string& /*class_type*/,
                                           const std::vector<std::string>& names,
                                           const OrderedJson& values,
                                           const OrderedJson& already_linked) {
    OrderedJson out = OrderedJson::object();
    std::size_t vi = 0;
    for (const std::string& name : names) {
        // 已经由连线提供的输入不再从控件取，**而且不消耗一个位置**。
        // 消耗掉的话后面所有参数错位一格。
        if (already_linked.contains(name)) continue;
        if (vi >= values.size()) break;
        out[name] = values[vi];
        ++vi;
        // 取完 seed 之后如果还有多余的值，那多半是伪控件，跳过。
        if ((name == "seed" || name == "noise_seed") && vi < values.size() &&
            is_seed_control_value(values[vi])) {
            ++vi;
        }
    }
    return out;
}

ApiWorkflow WorkflowConverter::convert(const OrderedJson& ui_workflow) {
    const auto nodes = ui_workflow.find("nodes");
    if (nodes == ui_workflow.end() || !nodes->is_array()) {
        throw WorkflowError(
            "这不是界面版工作流。如果已经是接口版，直接用 ApiWorkflow 包一层");
    }

    // 连线表：link_id -> (源节点 id, 源输出槽)
    std::map<long long, std::pair<std::string, long long>> link_src;
    const auto links = ui_workflow.find("links");
    if (links != ui_workflow.end() && links->is_array()) {
        for (const auto& link : *links) {
            if (!link.is_array() || link.size() < 5) continue;
            if (!link[0].is_number_integer()) continue;
            const long long id = link[0].get<long long>();
            std::string src;
            if (link[1].is_string()) {
                src = link[1].get<std::string>();
            } else if (link[1].is_number_integer()) {
                src = std::to_string(link[1].get<long long>());
            } else {
                continue;
            }
            const long long slot =
                link[2].is_number_integer() ? link[2].get<long long>() : 0;
            link_src[id] = {src, slot};
        }
    }

    OrderedJson prompt = OrderedJson::object();
    for (const auto& node : *nodes) {
        // 界面上被静音（2）或旁路（4）的节点不提交。
        const auto mode = node.find("mode");
        if (mode != node.end() && mode->is_number_integer()) {
            const long long m = mode->get<long long>();
            if (m == 2 || m == 4) continue;
        }

        const std::string nid = node_id_of(node);
        const auto type = node.find("type");
        if (type == node.end() || !type->is_string()) {
            throw WorkflowError("节点 " + nid + " 没有 type");
        }
        const std::string class_type = type->get<std::string>();

        OrderedJson inputs = OrderedJson::object();

        // 连线型输入
        const auto slots = node.find("inputs");
        if (slots != node.end() && slots->is_array()) {
            for (const auto& slot : *slots) {
                const auto link = slot.find("link");
                if (link == slot.end() || !link->is_number_integer()) continue;
                const auto found = link_src.find(link->get<long long>());
                if (found == link_src.end()) continue;
                const auto name = slot.find("name");
                if (name == slot.end() || !name->is_string()) continue;
                inputs[name->get<std::string>()] =
                    OrderedJson::array({found->second.first, found->second.second});
            }
        }

        // 控件型输入
        const auto values = node.find("widgets_values");
        if (values != node.end() && values->is_array() && !values->empty()) {
            const auto& names = widget_names(class_type);
            const OrderedJson zipped =
                zip_widgets(class_type, names, *values, inputs);
            for (const auto& kv : zipped.items()) inputs[kv.key()] = kv.value();
        }

        prompt[nid] = OrderedJson{{"class_type", class_type},
                                  {"inputs", inputs}};
    }

    return ApiWorkflow(std::move(prompt));
}

OrderedJson load_ui_workflow(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        throw WorkflowError("工作流文件不存在：" + paths::to_utf8(p));
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) throw WorkflowError("读不了工作流文件：" + paths::to_utf8(p));
    std::ostringstream buf;
    buf << in.rdbuf();

    OrderedJson doc = OrderedJson::parse(buf.str(), nullptr, false);
    if (doc.is_discarded()) {
        throw WorkflowError("工作流文件不是合法 JSON：" + paths::to_utf8(p));
    }
    return doc;
}

}  // namespace changji::comfy
