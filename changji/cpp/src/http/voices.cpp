#include "http/voices.hpp"

#include <nlohmann/json.hpp>

namespace changji::http {

using json = nlohmann::ordered_json;

ApiResult get_voices(const std::string& path, const std::string& backend) {
    // 项目路径照旧要：前端一直是带着它来的，突然不校验的话
    // 拼错路径的请求会静默成功，而错误在别的接口上才暴露出来。
    if (path.empty()) throw ApiError(400, "没有指定项目目录");

    // **两条后端都没有服务端清单，但原因不一样，说法也不该一样。**
    if (backend == "http") {
        return {200,
                {{"voices", json::array()},
                 {"error", "外部配音服务（[tts].backend = http）的音色由那个"
                           "服务自己管，这里问不到。把角色的 voice_id 填成"
                           "那个服务认的音色名就行。"}}};
    }
    return {200,
            {{"voices", json::array()},
             {"error", "进程内配音（[tts].backend = local）没有音色清单。"
                       "音色来自参考音频：把角色的 voice_id 填成一段人声"
                       "片段的路径，模型照着它的音色念。"}}};
}

}  // namespace changji::http
