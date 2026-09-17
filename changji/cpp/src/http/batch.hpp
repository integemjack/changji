#pragma once

// 两个长任务：连着写好几集、给还没分镜的剧集批量出分镜。
//
// 和前面五个接口的根本区别：**它们跑几分钟，不是几十秒**。
// 所以走 job 表——立刻返回 {"started": true, ...}，进度靠
// GET /api/script/series 轮询或者 WebSocket 推。
//
// 两个共用同一个任务槽（JobKind::Write）。Python 那边也是同一个
// WriteState，所以"写整季的时候不能同时批量出分镜"这条限制是照抄的，
// 不是我加的。

#include <memory>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"

namespace changji::http {

/// POST /api/script/series —— 连着写好几集，边写边存。
///
/// 跟单集不同，这个**直接落库并建出剧集**。一集一集手点新建再手点写，
/// 写到第五集人就放弃了，那量产就无从谈起。写完可以逐集再改。
///
/// 客户端用 shared_ptr 是因为任务在工作线程上跑，可能比这次请求活得久。
/// 传引用的话，调用方一不小心让它先析构，表现是工作线程访问已释放对象——
/// 而那时候栈上已经没有任何线索指向这里了。
ApiResult post_script_series(const nlohmann::json& body,
                             std::shared_ptr<llm::Client> client);

/// POST /api/story/chapters —— 把还没正文的章一口气全展开。
///
/// 十六章的故事逐章点十六次不像话，而每章要跑十几秒，加起来是分钟级的，
/// 所以和上面两个一样走 job 表。**共用同一个任务槽**，也就是说写整季、
/// 批量分镜、批量展开三件事同时只能做一件——它们都在跟同一个大模型排队。
///
/// **每写完一章就落库并重读**：下一章的提示词里「上一章是这么结束的」
/// 拿到的才是刚写完那一章。全写完再一次性存的话，中途停掉就全白干了，
/// 而且每一章都以为自己接的是空的上一章。
ApiResult post_story_chapters(const nlohmann::json& body,
                              std::shared_ptr<llm::Client> client);

/// POST /api/plan/all —— 把还没分镜的剧集一次补齐。
///
/// 连着写了五集之后，每一集都还得单独点一次「重出分镜」。
/// 五次里漏掉一次，跑整个项目时那一集就被跳过去了。
/// POST /api/script/all —— 把所有挂着章、又还没剧本的章一次改编完。
/// 和「批量补分镜」对称：那一步是它后面那一步。
ApiResult post_script_all(const nlohmann::json& body,
                          std::shared_ptr<llm::Client> client);

ApiResult post_plan_all(const nlohmann::json& body,
                        std::shared_ptr<llm::Client> client);

/// POST /api/story/understand —— 理解故事：一件活，把整本书读一遍，拍片
/// 要的全出来。
///
/// 用户 2026-09-17：设定页只留一颗「理解故事」，跑完人物、关系、场景、长相
/// 和剧本都有了；「出图」只是画。理解和定妆合成一件（两件都是把整本书读一
/// 遍，第二遍的输入就是第一遍的输出）；剧本一章一章接着写——一次调用写不出
/// 整部，第二章要看着第一章写。
///
/// 三步：读一遍（post_story_understand_once：结构和长相一次出来）→ 章对集
/// （存故事时自动做，这儿再对一次）→ 逐章写剧本（同 script/all 那个循环）。
/// 跑在 JobKind::Write 那个槽上，进度走 GET /api/script/series；能停，停了
/// 已经出来的留着。
///
/// overwrite=false：故事里已有人物表就不再读，剧本只补缺的。
/// overwrite=true：全部重来。
/// body: {project, overwrite?}
ApiResult post_story_understand(const nlohmann::json& body,
                                std::shared_ptr<llm::Client> client);

}  // namespace changji::http
