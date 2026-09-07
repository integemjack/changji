#pragma once

// 剧本读写和剧集增删改。
//
// 这几个接口属于阶段 3（编辑接口），当时漏了。漏的原因值得记一笔：
// 阶段 3 是按 test_web_editing.py 的覆盖面移植的，而那个文件不测这几个。
// **对拍语料的覆盖面成了移植的覆盖面**——只要拿测试文件当清单，
// 测试没覆盖的就会整片漏掉，而且漏得很干净，没有任何编译或运行迹象。
// 是阶段 2 判据的实机验证（前端调了 404）才把它们暴露出来的。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

/// 下一个没被占用的剧集编号。
///
/// **只数 epNN 那些。** 预告片挂在 trailer 上，把它也数进去的话，
/// 有了预告之后新建的第二集会跳号变成 ep03。
std::string next_episode_id(const models::Project& project);

/// GET /api/script —— 读某一集的剧本。
ApiResult get_script(const std::string& path, const std::string& episode_id);

/// POST /api/script —— 改剧本，可以顺便重出分镜。
///
/// 重出会**覆盖整张分镜表**，人工改过的镜头会丢，所以要显式勾 regenerate。
ApiResult post_script(const nlohmann::json& body, llm::Client& client,
                      pipeline::CancelToken& tok);

/// POST /api/episode —— 新建一集。id 留空就自动往后编号。
ApiResult post_episode(const nlohmann::json& body);

/// POST /api/episode/action —— 删除、复制或改名一集。
///
/// 复制只带走剧本和分镜的文案，**不带产出物和状态**。复制出来的一集
/// 是要重跑的，把状态也带过去会让它显示成已完成但没有文件。
ApiResult post_episode_action(const nlohmann::json& body);

}  // namespace changji::http
