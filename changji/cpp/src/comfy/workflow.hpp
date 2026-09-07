#pragma once

// 工作流格式转换与参数注入。
//
// ComfyUI 有两种工作流格式，这是新手最容易栽的地方：
//
// **界面版**（用户在浏览器里保存的那种）是 `{"nodes": [...], "links": [...]}`，
// 节点的参数放在 `widgets_values` 数组里，**只有位置没有名字**。
//
// **接口版**（POST /prompt 要的那种）是
// `{"节点id": {"class_type": ..., "inputs": {...}}}`，参数是有名字的键值对。
//
// 两者之间的映射关系**不在文件里**，必须从服务端的 `/object_info` 拿。
// 硬编码一张映射表在 ComfyUI 升级后就会错，所以这里从服务端动态取。
//
// 移植自 src/changji/comfy/workflow.py。
//
// ⚠️ 这里所有 JSON 都用 **ordered_json**，不是 json。
// `/object_info` 里 `input.required` 的**键顺序就是界面上控件的顺序**，
// 而控件值是按位置对上去的。用普通 json（按键排序）的话顺序全乱，
// 表现是每个参数都被塞进了别人的位置——而那不会报错，只会出一张莫名其妙的图。

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::comfy {

using OrderedJson = nlohmann::ordered_json;

class WorkflowError : public std::runtime_error {
public:
    explicit WorkflowError(const std::string& what) : std::runtime_error(what) {}
};

/// 接口格式的工作流。可以按节点类型定位并改参数。
class ApiWorkflow {
public:
    explicit ApiWorkflow(OrderedJson prompt) : prompt_(std::move(prompt)) {}

    const OrderedJson& to_json() const { return prompt_; }
    OrderedJson& to_json() { return prompt_; }

    /// 按节点类型找出所有节点 id。顺序是工作流里的顺序。
    std::vector<std::string> find_by_class(const std::string& class_type) const;

    /// 按节点类型找唯一节点。**不唯一就报错**——改错地方比不改更糟：
    /// 一个工作流里两个 KSampler 是常见的（高噪声段和低噪声段），
    /// 随便挑一个改，出来的片子只有一半参数是对的。
    std::string one_by_class(const std::string& class_type) const;

    void set_input(const std::string& node_id, const std::string& key,
                   OrderedJson value);

    /// 定位唯一节点并批量改参数。返回节点 id。
    std::string set_by_class(
        const std::string& class_type,
        const std::vector<std::pair<std::string, OrderedJson>>& kv);

    /// 取不到返回 null（不是抛异常）。
    OrderedJson get_input(const std::string& node_id,
                          const std::string& key) const;

private:
    OrderedJson prompt_;
};

/// 把界面版工作流转成接口版。
///
/// 需要服务端的 `/object_info` 才能知道每个节点的控件顺序和名字。
class WorkflowConverter {
public:
    explicit WorkflowConverter(OrderedJson object_info)
        : object_info_(std::move(object_info)) {}

    /// 取一个节点类型的控件名，**按界面上的顺序**。
    ///
    /// `/object_info` 里 input.required 是有序的，顺序和界面上控件的顺序一致。
    /// 连线型输入（MODEL、CLIP 这些）不占 widgets_values 的位置，要排除。
    const std::vector<std::string>& widget_names(const std::string& class_type);

    ApiWorkflow convert(const OrderedJson& ui_workflow);

private:
    /// 把 widgets_values 数组对回控件名。
    ///
    /// 难点是界面会在数组里插入**伪控件**。最典型的是 KSampler：界面上
    /// seed 后面跟着一个 control_after_generate 下拉框，它占了数组的一个位置，
    /// 但接口不认这个参数。不跳过它，后面 steps、cfg、sampler 全部错位一格——
    /// 而错位之后工作流照样能提交、照样能跑，只是出来的东西不对。
    OrderedJson zip_widgets(const std::string& class_type,
                            const std::vector<std::string>& names,
                            const OrderedJson& values,
                            const OrderedJson& already_linked);

    OrderedJson object_info_;
    std::map<std::string, std::vector<std::string>> widget_names_;
};

/// 判断一个输入是控件（占 widgets_values 一格）还是连线。
///
/// **必须用白名单判断标量类型**：INT 和 FLOAT 也是全大写，
/// 但它们是控件不是连线。按"全大写就是连线"判断的话，
/// 每个 KSampler 的 steps 和 cfg 都会被当成连线丢掉。
bool is_widget_type(const OrderedJson& type_def);

/// 读界面版工作流文件。
OrderedJson load_ui_workflow(const std::filesystem::path& p);

}  // namespace changji::comfy
