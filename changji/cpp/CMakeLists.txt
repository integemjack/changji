cmake_minimum_required(VERSION 3.20)
project(changji C CXX)
# C 也要启用：ggml 是 C 写的，而且 sd.cpp 声明了 target_compile_features(c_std_11)。
# 只写 CXX 的话 configure 会在 generate 阶段报 "No known features for C compiler"，
# 而那句话看不出和缺 C 语言有关系。

# 全局停在 C++17，只把自己的目标抬到 20。
#
# 原本这里写的是 CMAKE_CXX_STANDARD 20。前置验证工程实测发现那样到阶段 5
# 会炸：llama.cpp 和 ggml 写的是 target_compile_features(... cxx_std_17)
# 加一句 "# don't bump"，而那声明的是**下限不是上限**——父作用域给 20 时
# "至少 17"照样满足，于是上游整个按 C++20 编，然后在 MSVC 上撞 char8_t
# （llama-chat.cpp 的 LU8 宏用 __cplusplus 做守卫，而 MSVC 不加
# /Zc:__cplusplus 时那个宏永远是 199711L，守卫静默失效）。
# 只在 MSVC + C++20 这个组合下出现，MinGW 编不出来，所以阶段 0 没遇到。
#
# 详见 verify/RESULTS.md。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 静态 CRT 要在**拉依赖之前**定，它得传染给所有子项目——
# 一半 /MT 一半 /MD 的话链接时报 RuntimeLibrary 不匹配，
# 而那个错误信息指向的是某个 .obj，看不出是运行库选项的问题。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    # ggml 默认开 OpenMP，那会拖进 VCOMP140.DLL——**MSVC 的 OpenMP 运行时
    # 只有动态版**，/MT 也去不掉它。关掉之后 ggml 用自带的线程池，
    # CPU 推理会慢一点，但这个项目的目标是 GPU 推理，
    # 而"拷过去就能跑"比那点 CPU 性能重要。
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- 依赖 ----
#
# 全部走 FetchContent 而不是 vcpkg/conan：交叉编译到树莓派时，
# 包管理器的三元组配置是另一套要维护的东西，而这几个库都是
# header-only 或极小的，直接拉源码编最省心。
#
# 版本一律钉死。这几个库都在活跃开发，不钉版本会出现
# "昨天能编今天编不过" 的情况。

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON)

FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW ON)

FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW ON)

# Crow 依赖 asio。用 standalone 版而不是 boost 版，
# 少拉一个巨大的依赖，交叉编译也简单得多。
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(asio)
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH "" FORCE)

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.0
    GIT_SHALLOW ON)

FetchContent_MakeAvailable(json tomlplusplus httplib Crow)

# Crow 1.2.0 的状态码表里没有 422，会把它改写成 500。
# 所有 pydantic 风格的校验错误都受影响，而单元测试看不见——
# 那边不经过 Crow 的写响应那一步。详见脚本里的注释。
include("${CMAKE_CURRENT_LIST_DIR}/cmake/patch_crow_422.cmake")
changji_patch_crow_422("${crow_SOURCE_DIR}/include")

# ⚠️ **httplib 的头文件目录不能是 SYSTEM。**
#
# sd.cpp 把自己的 thirdparty 目录（里面有一份 httplib.h 0.28.0）作为
# INTERFACE 包含目录导出，那是普通的 -I。而 MSVC 的 SYSTEM 目录
# （-external:I）**永远排在普通 -I 之后**——所以只要 httplib 是 SYSTEM，
# sd.cpp 那份就必赢，我们钉的 0.15.3 一行都用不上。
#
# 这不是假想：llm/client_http.cpp 和 doctor.cpp 从链上 sd.cpp 那天起
# 编的一直是 0.28.0。两边碰巧 API 兼容，所以从来没报过错，直到 comfy
# 这边用到 multipart 才炸出来（0.28.0 把 MultipartFormDataItems 改了名）。
# **一个版本号写在构建脚本里、实际编的是另一个版本**，比编不过更危险。
#
# 清掉 SYSTEM 标记之后它就是普通 -I，再配合下面的 BEFORE 排到最前面。
# 代价是 httplib 的警告会进 /W4 的视野，所以用到它的地方包一层
# util/httplib.hpp 把警告按下去。
if(TARGET httplib)
    set_property(TARGET httplib PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
endif()

# ---- stable-diffusion.cpp ----
#
# 阶段 5 的核心依赖。出首帧、出视频都靠它。
#
# **暂时不统一 ggml。** 方案的风险二说统一 ggml 是一笔长期维护成本，
# 但那笔账只在 sd.cpp 和 llama.cpp **同时链接**时才需要付——两个库各自
# vendor 了不同 commit 的 ggml，同时进来才有符号冲突。
#
# 决策 4 之后大模型走远端（[llm] 配置段），llama.cpp 还没进程内化，
# 所以现在 sd.cpp 可以带着自己那份 ggml 单独链。patches/ 下的补丁集
# 留着不动，等真要把 llama.cpp 链进来时再用。
#
# 这不是把问题推给未来：补丁集已经在 CUDA 和 Metal 上都验证过了
# （见 verify/RESULTS.md），只是现在用不上。提前统一等于现在就开始付
# 那笔"两个上游各自前进"的维护成本，而收益要等到 llama.cpp 进来才有。
option(CHANGJI_SD "链接 stable-diffusion.cpp" ON)
option(CHANGJI_SD_CUDA "sd.cpp 用 CUDA 后端" OFF)

# 编给哪些显卡架构。**必须显式给**。
#
# 不给的话交由 CMake/nvcc 的默认值决定，而默认值经常不含新卡的架构。
# 那时候不会报错——CUDA 退回 PTX JIT，第一次跑每个内核现编一遍。
# 表现是"能跑，但莫名其妙地慢"，而且慢在哪儿完全看不出来。
#
# 常见的几张：
#   L20 / L40S / RTX 4090   → 89（Ada）
#   A100                    → 80
#   H100                    → 90
#   RTX 3090 / A10          → 86
#   RTX 2060（开发机）       → 75（Turing）
#
# 多种卡混着用就写 "75;86;89"，编译时间和产物大小会跟着涨。
# `native` 只编当前机器上那张卡的——**在容器里构建时别用**，
# 构建机和跑的机器不一定是同一张卡。
set(CHANGJI_CUDA_ARCH "89" CACHE STRING
    "CUDA 架构号，分号隔开。L20/L40S/4090 是 89，A100 是 80，H100 是 90")

# ---- llama.cpp（阶段 9：进程内 TTS）----
#
# **默认关。** 开着会让每一次干净构建多编一份 llama.cpp + 一份打过补丁的
# ggml，而阶段 8 之前没有哪条主线功能用得到它。
#
# 为什么现在把它接进来：Qwen3-TTS 已经进了 llama.cpp 主线
# （PR #26254，2026-08-04 合并，走 mtmd），配音的 C++ 化不再需要
# 自己写 codec，但需要 llama.cpp 在进程里。见方案风险一那一节。
#
# **两份 ggml 的问题在前置验证工程里已经解掉了**（决策 17）：
# 统一的那份 ggml 是"llama.cpp 自带的 + leejet 扩展补丁"，
# CPU 和 CUDA 两条链都验过（verify/RESULTS.md）。这里做的是把那套
# 手工步骤自动化——verify/ 是一次性工程，人可以在中间插一次手；
# 主工程靠 FetchContent 拉源码，没人插得进去。
option(CHANGJI_LLAMA "链接 llama.cpp（进程内 TTS）" OFF)

# **GGML_MAX_NAME 要在顶层定义，链了 ggml 就要，不管它是谁带进来的。**
#
# sd.cpp 自己的 CMakeLists 里有 `add_definitions(-DGGML_MAX_NAME=160)`，
# 但 add_definitions 只作用于**它之后的目录作用域**——加到了 ggml 上，
# 没加到我们自己的源文件上。于是默认构建里库是 160、本体是 64，
# **编译和链接全都不报错**。
#
# 这不是推理出来的：doctor 的 ggml ABI 探针接上之后第一次跑就报了 fail，
# "库是按 160 编的，本体是 64"。在此之前它已经这样很久了，
# 只是本体里没有人直接读写 ggml_tensor 的字段，所以还没出事。
if(CHANGJI_SD OR CHANGJI_LLAMA)
    add_compile_definitions(GGML_MAX_NAME=160)
endif()

if(CHANGJI_LLAMA)
    # 上面那行原来在这个块里。留着这段注释是因为它解释了后果：
    # sd.cpp 自己的 CMakeLists 里有 `add_definitions(-DGGML_MAX_NAME=160)`，
    # 而 add_definitions 只作用于它之后的目录作用域——加不到别处已经编好的
    # ggml 上。结果是 ggml 按 64 编、sd.cpp 按 160 编，两边看到的 ggml_tensor
    # 大小不一致，**链接不报错**。所以在顶层统一定义。
    # verify/main.cpp 的 check_ggml_abi() 是这一条的运行时探针：
    # 写一个 100 字符的张量名再读回来，读回 63 就说明这行没生效。

    # 打补丁要一个 Python。找不到就当场停，不要配置期悄悄过、
    # 编译期报一堆"找不到 GGML_TYPE_F8_E4M3"——那时候没人会想到是这儿。
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    foreach(opt LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES
                LLAMA_BUILD_SERVER LLAMA_BUILD_APP
                LLAMA_BUILD_UI LLAMA_OPENSSL)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()

    # **common 要开。** 上面那批全关掉时它也在里面，本来是对的——
    # 配音只要 mtmd。但进程内跑文本大模型要**按 JSON Schema 约束输出**，
    # 而 `json_schema_to_grammar` 只在 common 里，libllama 的公开头文件里
    # 只有吃 GBNF 文本的 `llama_sampler_init_grammar`。
    #
    # 不约束的后果不是"偶尔格式不对"：整条流水线是无人值守跑的，
    # 一次解析失败就是一集卡住，而模型下一次未必吐得更好。
    #
    # mtmd 那边**不能**链 common（它自己的 CMakeLists 里有断言），
    # 我们链在 changji 上，两者不冲突。
    set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)

    # **配音要 mtmd，而 mtmd 在 tools/ 底下。**
    # Qwen3-TTS 的实现（models/qwen3tts-gen.cpp、qwen3tts-spkenc.cpp）
    # 在 mtmd 里，不在 libllama 里。上游正好留了这个钩子：
    # LLAMA_BUILD_MTMD 单独编 mtmd，不用把整个 tools/ 和 common 拖进来。
    set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)

    # **视频支持关掉。** mtmd 的 MTMD_VIDEO 要 LLAMA_SUBPROCESS，
    # 而且运行时要 PATH 里有 ffmpeg 二进制。我们用不到（配音只出音频），
    # 开着等于给二进制加一个用不上的运行时依赖。
    set(MTMD_VIDEO OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        # 钉死，理由同 sd.cpp。这个 commit 是前置验证工程验过的那个。
        GIT_TAG 5202104
        GIT_SHALLOW ON
        # 拉下来就地打补丁。脚本是幂等的——FetchContent 什么时候重跑
        # PATCH_COMMAND 并不好预测，而 git apply 打第二遍会失败。
        PATCH_COMMAND ${CMAKE_COMMAND} -E env
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_to_llamacpp.py"
            "<SOURCE_DIR>")
    FetchContent_MakeAvailable(llama)
endif()

if(CHANGJI_SD)
    if(CHANGJI_LLAMA)
        set(CHANGJI_SD_SUBMODULES "")
    else()
        set(CHANGJI_SD_SUBMODULES "ggml")
    endif()

    # 只要 ggml 那个子模块。webui 是个前端仓库，libwebp/libwebm 我们关掉了
    # 对应的功能——全递归拉下来是几百兆，而且拉一个前端仓库来编后端很荒唐。
    set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SD_WEBP OFF CACHE BOOL "" FORCE)
    set(SD_WEBM OFF CACHE BOOL "" FORCE)
    set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    if(CHANGJI_SD_CUDA)
        set(SD_CUDA ON CACHE BOOL "" FORCE)
        # **llama.cpp 的 ggml 走的是另一个开关。**
        # 开了 CHANGJI_LLAMA 时 ggml 由 llama.cpp 提供，而它的 CUDA 后端
        # 由 GGML_CUDA 控制，不认 SD_CUDA。只设 SD_CUDA 的话编出来的是
        # 一个**完全没有 CUDA 的**二进制——不报错，全在 CPU 上跑，
        # 表现就是"能跑但莫名其妙地慢"。
        # 实测过：那一版 GGML_CUDA:BOOL=OFF，连 libggml-cuda.a 都没有。
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
        # ggml 读的是 CMAKE_CUDA_ARCHITECTURES。在拉它之前设好，
        # 拉完再设就晚了——那时候目标已经建出来了。
        set(CMAKE_CUDA_ARCHITECTURES "${CHANGJI_CUDA_ARCH}" CACHE STRING "" FORCE)

        # **配置期就要确认 nvcc 找得到，找不到当场停。**
        #
        # 不停的话整条链子一声不吭：CMake 配置成功、编译成功、服务起得来、
        # 任务开跑——只是 ggml 的 CUDA 后端根本没编进去，全部退到 CPU。
        # 2026-09-10 在 5090 上栽过：手敲配置命令漏了
        # `-DCMAKE_CUDA_COMPILER`，而那台机器的 nvcc 不在 PATH 上。
        # 结果是 1280×704 的一镜跑了五个多小时才两步，进程占着 52 GB 内存、
        # 395% CPU，而 `nvidia-smi` 上一个计算进程都没有。
        # 查出来靠的是 `strings 二进制 | grep -c ggml_cuda` —— 0 对 2348。
        #
        # 已经设过 CMAKE_CUDA_COMPILER 的话就信它（交叉编译、多版本共存
        # 都可能指到非默认位置）；没设就自己找一遍常见路径。
        if(NOT CMAKE_CUDA_COMPILER)
            find_program(CHANGJI_NVCC nvcc
                HINTS ENV CUDA_PATH ENV CUDA_HOME
                PATHS /usr/local/cuda/bin /usr/local/cuda-12/bin
                      /usr/local/cuda-13/bin
                PATH_SUFFIXES bin)
            if(CHANGJI_NVCC)
                set(CMAKE_CUDA_COMPILER "${CHANGJI_NVCC}" CACHE STRING "" FORCE)
                message(STATUS "CUDA 编译器：${CHANGJI_NVCC}（自动找到的）")
            else()
                message(FATAL_ERROR
                    "开了 CHANGJI_SD_CUDA 但找不到 nvcc。\n"
                    "**不能就这么编下去**：编出来的二进制没有 CUDA 后端，"
                    "不报错、只是全在 CPU 上跑，慢一百倍而且很难看出原因。\n"
                    "  装了 CUDA 的话显式指一下："
                    "-DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc\n"
                    "  真不想用 CUDA 就关掉：-DCHANGJI_SD_CUDA=OFF")
            endif()
        endif()

        message(STATUS "CUDA 架构：${CHANGJI_CUDA_ARCH}"
                       "（不对的话会退回 PTX JIT，不报错但很慢）")
    endif()

    FetchContent_Declare(stable_diffusion_cpp
        GIT_REPOSITORY https://github.com/leejet/stable-diffusion.cpp.git
        # 钉死。ggml 和 sd.cpp 都在活跃开发，不钉版本会出现
        # "昨天能编今天编不过"，而这两个的编译时间以分钟计。
        GIT_TAG 9cdb6b6fc05edf2d446bfe881a4c462b241b3025
        # 开了 CHANGJI_LLAMA 的时候不拉 ggml 子模块：ggml 目标已经由
        # llama.cpp 建好了，sd.cpp 的 CMakeLists 写着 `if (NOT TARGET ggml)`，
        # 会自己跳过。拉了也是白拉几百兆。
        GIT_SUBMODULES "${CHANGJI_SD_SUBMODULES}"
        GIT_SHALLOW OFF)
    FetchContent_MakeAvailable(stable_diffusion_cpp)

    if(CHANGJI_LLAMA)
        # **sd.cpp 用相对路径拿 ggml 的内部头**：三个 .cpp 里写着
        # `#include "ggml/src/ggml-impl.h"`，那是相对它自己那个 ggml 子模块的。
        # 我们没拉那个子模块（ggml 由 llama.cpp 提供），所以要把 llama.cpp
        # 的源码根加进搜索路径——它底下正好也是 ggml/src/ggml-impl.h。
        #
        # 前置验证工程没撞上这个：它钉的 sd.cpp 是 d8fb10c(master-846)，
        # 而主工程钉的是 9cdb6b6(master-848)，中间那两个提交新增了
        # compute_workspace.cpp。**"验证工程验过"不等于"换个版本还成立"。**
        #
        # 头和库必须同源：库是 llama.cpp 那份打过补丁的 ggml，头也得是它，
        # 否则又回到 GGML_MAX_NAME 那类"编得过、跑起来踩内存"的问题。
        target_include_directories(stable-diffusion PRIVATE "${llama_SOURCE_DIR}")
    endif()
endif()

# ---- 本体 ----

add_executable(changji
    src/main.cpp
    src/util/paths.cpp
    src/util/proc.cpp
    src/net/ws_client.cpp
    src/config/settings.cpp
    src/config/writeback.cpp
    src/config/runtime.cpp
    src/doctor/doctor.cpp
    src/models/shot.cpp
    src/models/character.cpp
    src/models/project.cpp
    src/models/hardware.cpp
    src/util/text.cpp
    src/util/human_time.cpp
    src/util/fs_time.cpp
    src/stages/json_extract.cpp
    src/stages/bible.cpp
    src/stages/storyboard.cpp
    src/stages/script.cpp
    src/stages/prompt_compose.cpp
    src/stages/frames.cpp
    src/stages/render.cpp
    src/stages/audio_plan.cpp
    src/stages/audio.cpp
    src/stages/tts_backends.cpp
    src/infer/ggml_abi.cpp
    src/infer/llama_tts.cpp
    src/infer/llama_chat.cpp
    src/infer/scheduler.cpp
    src/infer/sd_backend.cpp
    src/infer/sd_image.cpp
    src/infer/sd_video.cpp
    src/infer/sd_upscale.cpp
    src/llm/client.cpp
    src/llm/local_client.cpp
    src/llm/client_http.cpp
    src/infer/worker_pool.cpp
    src/infer/worker_server.cpp
    src/infer/worker_proto.cpp
    src/http/flow.cpp
    src/http/webapp.cpp
    src/http/ws.cpp
    src/pipeline/jobs.cpp
    src/pipeline/episode.cpp
    src/media/ffmpeg.cpp
    src/media/subtitles.cpp
    src/gates/checks.cpp
    src/media/assemble.cpp
    src/http/readonly.cpp
    src/http/editing.cpp
    src/http/editing_assets.cpp
    src/http/editing_batch.cpp
    src/http/media.cpp
    src/http/upload.cpp
    src/http/scripting.cpp
    src/http/planning.cpp
    src/http/batch.cpp
    src/http/episodes.cpp
    src/http/projects.cpp
    src/http/llm_info.cpp
    src/http/config_api.cpp
    src/http/config_api_doctor.cpp
    src/http/reset.cpp
    src/http/run.cpp
    src/http/voices.cpp
    src/http/run_deps.cpp
    src/http/server.cpp
)

# 自己的代码要 C++20，但只加在自己的目标上，不传染给上游。
# 阶段 5 链 sd.cpp / llama.cpp 时这一点是必须的，见上面 CMAKE_CXX_STANDARD 那段。
target_compile_features(changji PRIVATE cxx_std_20)

target_include_directories(changji PRIVATE
    src
    "${ASIO_INCLUDE_DIR}"
)
# 我们那份 httplib 排在最前面，见上面 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES 那段。
target_include_directories(changji BEFORE PRIVATE "${httplib_SOURCE_DIR}")

target_link_libraries(changji PRIVATE
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    httplib::httplib
    Crow::Crow
)

target_compile_definitions(changji PRIVATE
    ASIO_STANDALONE
    # cpp-httplib 默认要 OpenSSL 才能发 https。阶段 0 只连本机 http 服务，
    # 先不引 OpenSSL。等真要连云端 LLM 时再打开 CPPHTTPLIB_OPENSSL_SUPPORT。
)

if(CHANGJI_LLAMA)
    # mtmd 而不只是 llama：Qwen3-TTS 的实现在 mtmd 里。
    # mtmd 的 PUBLIC 链接里已经带上了 llama 和 ggml。
    # mtmd 带来 llama 和 ggml；llama-common 是为了 json_schema_to_grammar。
    target_link_libraries(changji PRIVATE mtmd llama-common)
    target_compile_definitions(changji PRIVATE CHANGJI_HAVE_LLAMA=1)
    # ggml 的头。开了 CHANGJI_LLAMA 时它由 llama.cpp 提供，
    # 而 doctor 的 ABI 探针要 include "ggml.h"。
    target_include_directories(changji PRIVATE "${llama_SOURCE_DIR}/ggml/include")
    # mtmd.h 在 tools/mtmd 下。mtmd 目标自己 PUBLIC 了这个目录，
    # 这一行是为了万一以后改成别的链接方式时不至于悄悄编不到。
    target_include_directories(changji PRIVATE "${llama_SOURCE_DIR}/tools/mtmd")
endif()

if(CHANGJI_SD)
    target_link_libraries(changji PRIVATE stable-diffusion)
    target_compile_definitions(changji PRIVATE CHANGJI_HAVE_SD=1)
    # 没开 CHANGJI_LLAMA 时 ggml 是 sd.cpp 自带的那份，头在它的子模块里。
    if(NOT CHANGJI_LLAMA)
        target_include_directories(changji PRIVATE
            "${stable_diffusion_cpp_SOURCE_DIR}/ggml/include")
    endif()
    # stb 那两个头在 sd.cpp 的 thirdparty 里，它自己没 export 这个目录。
    # 用它们是为了在**内存里**编解码 PNG——stb 自带的 fopen 版本在
    # Windows 上把路径按 ANSI 代码页解释，中文项目名直接写不进去。
    # ⚠️ **不要把 sd.cpp 的 thirdparty 目录摊在包含路径上。**
    #
    # 那里面有一份 httplib.h（0.28.0）和一份 json.hpp，会盖住我们钉的版本。
    # 而且盖得**必然**：httplib::httplib 的头文件是以 SYSTEM 身份进来的
    # （MSVC 上是 -external:I），SYSTEM 目录永远排在普通 -I 之后。
    # 于是 llm/client_http.cpp 和 doctor.cpp 一直编的是 0.28.0，不是 0.15.3——
    # 两边碰巧 API 兼容所以从来没报过错，直到 comfy 这边用到 multipart
    # （0.28.0 把 MultipartFormDataItems 改了名）才炸出来。
    #
    # 现在只把 sd.cpp 的源码根目录留在路径上，stb 走
    # `#include <thirdparty/stb_image.h>`。多打几个字，换的是
    # "谁也不会再意外捡到别人的 httplib"。
endif()

if(WIN32)
    target_compile_definitions(changji PRIVATE
        _WIN32_WINNT=0x0A00   # Windows 10。asio 不给这个宏会警告并退化行为
        NOMINMAX              # windows.h 的 min/max 宏会和 std::min/max 打架
        WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE      # 路径要走宽字符，中文目录名才不会乱码
    )
    # shell32：CommandLineToArgvW（paths::utf8_args 用它拿宽字符参数）
    target_link_libraries(changji PRIVATE ws2_32 mswsock shell32)
    if(MSVC)
        # /utf-8       源码是 UTF-8，中文注释和中文字符串字面量都靠它
        # /Zc:__cplusplus
        #     不加这个的话 MSVC 的 __cplusplus 永远是 199711L，不管实际用的
        #     是哪个标准。后果有两层：一是自己的代码（doctor 报的 C++ 版本）
        #     显示成 "C++97"；二是任何拿 __cplusplus 做条件编译的第三方头文件
        #     都会在 MSVC 上静默走错分支——llama.cpp 的 LU8 宏就是这么炸的，
        #     见 verify/RESULTS.md。
        target_compile_options(changji PRIVATE /utf-8 /Zc:__cplusplus /W4)
    endif()
else()
    target_compile_options(changji PRIVATE -Wall -Wextra -Wpedantic)
    find_package(Threads REQUIRED)
    target_link_libraries(changji PRIVATE Threads::Threads)
endif()

# 静态链接 GCC 运行时。默认打开，因为"单一二进制、零运行时依赖"
# 是这个后端存在的理由之一：
#
#   Windows: 不静态链接的话，exe 要 libstdc++-6.dll、libgcc_s_seh-1.dll、
#            libwinpthread-1.dll 三个 MinGW 的 DLL 才能起来，
#            拷到没装 MinGW 的机器上直接闪退。
#   树莓派:  省掉目标机 GCC 版本不匹配导致的 "GLIBCXX_3.4.xx not found"。
#
# MSVC 走 /MT 是另一套机制，这些 flag 对它无效，所以只对 GNU/Clang 生效。
option(CHANGJI_STATIC_RUNTIME "静态链接 libstdc++/libgcc" ON)

# MSVC 走 /MT（静态 CRT），机制和 GCC 那套完全不同，所以单独一段。
#
# 不做的话 exe 依赖 MSVCP140.dll、VCRUNTIME140.dll 那一串，
# 拷到没装 VC++ 运行库的机器上直接弹"缺少 DLL"。而"零运行时依赖"
# 是这个后端的立项理由之一。
#
# **必须在 FetchContent 之前设**：这个变量要传染给所有子项目，
# 一半 /MT 一半 /MD 链接时会报 RuntimeLibrary 不匹配。
# 这里放在文件末尾只是为了和上面那段放一起读——CMake 的
# CMAKE_MSVC_RUNTIME_LIBRARY 是在生成阶段读的，位置不影响。
if(CHANGJI_STATIC_RUNTIME AND MSVC)
    set_property(TARGET changji PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(CHANGJI_STATIC_RUNTIME AND NOT MSVC)
    if(WIN32)
        # MinGW 上必须用 -static 而不是 -static-libgcc -static-libstdc++。
        # 后者漏掉 libwinpthread-1.dll——std::mutex 和 std::thread 走的就是它，
        # 结果是 exe 在开发机上能跑，拷到别的机器上闪退且没有任何提示。
        target_link_options(changji PRIVATE -static)
    else()
        # Linux 上不做全静态：glibc 静态链接会在 getaddrinfo 这类
        # 依赖 NSS 的调用上出问题，而我们要连 LLM 和推理服务，全是域名解析。
        target_link_options(changji PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

install(TARGETS changji RUNTIME DESTINATION bin)


# ---- 测试 ----
#
# doctest 而不是 Catch2：单头文件、编译快。树莓派上本机编译时 Catch2 的
# 编译开销受不了（Pi 5 编 Crow 那几个头文件已经很吃力了）。
option(CHANGJI_BUILD_TESTS "编单元测试" ON)

if(CHANGJI_BUILD_TESTS)
    enable_testing()

    # 只取源码，不跑它的 CMakeLists。
    #
    # doctest v2.4.11 的 CMakeLists 头一行是 cmake_minimum_required(VERSION 3.0)，
    # 而 CMake 4 把 "低于 3.5" 从警告改成了硬错误，配置直接失败。
    # 但 doctest 是单头文件，压根不需要它的构建脚本——SOURCE_SUBDIR 指向一个
    # 不存在的目录，FetchContent 就只下载不 add_subdirectory。
    #
    # 比 CMAKE_POLICY_VERSION_MINIMUM 那种全局降级干净：那个会把所有依赖的
    # 策略一起放宽，掩盖别的问题。
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW ON
        SOURCE_SUBDIR does-not-exist)
    FetchContent_MakeAvailable(doctest)

    add_executable(changji_tests
        tests/unit/main.cpp
        tests/unit/test_worker_proto.cpp
        tests/unit/test_worker_roster.cpp
        tests/unit/test_webapp.cpp
        tests/unit/test_shot.cpp
        tests/unit/test_character.cpp
        tests/unit/test_project.cpp
        tests/unit/test_hardware.cpp
        tests/unit/test_readonly.cpp
        tests/unit/test_editing.cpp
        tests/unit/test_editing_assets.cpp
        tests/unit/test_editing_batch.cpp
        tests/unit/test_reset.cpp
        tests/unit/test_media.cpp
        tests/unit/test_upload.cpp
        tests/unit/test_jobs.cpp
        tests/unit/test_models_config.cpp
        tests/unit/test_bible.cpp
        tests/unit/test_storyboard.cpp
        tests/unit/test_scheduler.cpp
        tests/unit/test_script.cpp
        tests/unit/test_llm_client.cpp
        tests/unit/test_scripting.cpp
        tests/unit/test_planning.cpp
        tests/unit/test_batch.cpp
        tests/unit/test_episodes.cpp
        tests/unit/test_llm_info.cpp
        tests/unit/test_writeback.cpp
        tests/unit/test_config_api.cpp
        tests/unit/test_projects.cpp
        tests/unit/test_prompt_compose.cpp
        tests/unit/test_sd_image.cpp
        tests/unit/test_frames.cpp
        tests/unit/test_render.cpp
        tests/unit/test_sd_video.cpp
        tests/unit/test_episode.cpp
        tests/unit/test_run.cpp
        tests/unit/test_ws_client.cpp
        tests/unit/test_ws_hub.cpp
        tests/unit/test_util_time.cpp
        tests/unit/test_proc.cpp
        tests/unit/test_paths.cpp
        tests/unit/test_facades.cpp
        tests/unit/test_voices.cpp
        tests/unit/test_ffmpeg.cpp
        tests/unit/test_subtitles.cpp
        tests/unit/test_gates.cpp
        tests/unit/test_assemble.cpp
        tests/unit/test_audio_plan.cpp
        tests/unit/test_audio.cpp
        tests/unit/test_tts_backends.cpp
        tests/unit/test_compat_diff.cpp
        tests/compat/diff.cpp

        # 被测的源码。刻意不包含 doctor.cpp、http/server.cpp、
        # llm/client_http.cpp、comfy/client_http.cpp 和 http/run_deps.cpp ——
        # 它们链 httplib、asio 或 Crow，而各自的逻辑部分都已经拆到
        # 不依赖网络库的文件里了。
        src/infer/worker_proto.cpp
        src/http/flow.cpp
        src/http/webapp.cpp
        src/models/shot.cpp
        src/models/character.cpp
        src/models/project.cpp
        src/models/hardware.cpp
        src/util/paths.cpp
        src/util/proc.cpp
        src/net/ws_client.cpp
        src/util/text.cpp
        src/util/human_time.cpp
        src/util/fs_time.cpp
        src/stages/json_extract.cpp
        src/stages/bible.cpp
        src/stages/storyboard.cpp
        src/stages/script.cpp
        src/stages/prompt_compose.cpp
        src/stages/frames.cpp
        src/stages/render.cpp
        src/stages/audio_plan.cpp
        src/stages/audio.cpp
        src/stages/tts_backends.cpp
        # 测试目标编的是没开 CHANGJI_LLAMA 的那份存根（返回"没编进来"）。
        # 少了它 tts_backends.cpp 里的 local_tts_backend 会链不上。
        # ws.cpp 进得来：它只 include <crow/websocket.h>（头文件库），
        # 不碰 asio 的运行时，也不链 Crow 的 app。Hub 里那个
        # connection* 纯粹当键用，从不解引用——所以拿假指针就能测。
        src/http/ws.cpp
        # 体检的"当前配置用得上哪些依赖"是纯判断，不碰网络，所以进得来。
        # doctor.cpp 本身仍然不进——它链 httplib。
        # 这两个都是可选依赖的门面，测试目标里编的是没链上游时那一支
        # （CHANGJI_HAVE_SD / CHANGJI_HAVE_LLAMA 只给 changji 目标定义）。
        # test_facades.cpp 验的就是那一支的行为。
        src/infer/ggml_abi.cpp
        src/infer/llama_tts.cpp
        src/infer/llama_chat.cpp
        src/infer/scheduler.cpp
        src/infer/sd_backend.cpp
        src/infer/sd_image.cpp
        src/infer/sd_video.cpp
        src/infer/sd_upscale.cpp
        src/pipeline/jobs.cpp
        src/pipeline/episode.cpp
        src/media/ffmpeg.cpp
        src/media/subtitles.cpp
        src/gates/checks.cpp
        src/media/assemble.cpp
        src/llm/client.cpp
        src/llm/local_client.cpp
        src/config/settings.cpp
        src/config/writeback.cpp
        src/config/runtime.cpp
        src/http/readonly.cpp
        src/http/editing.cpp
        src/http/editing_assets.cpp
        src/http/editing_batch.cpp
        src/http/media.cpp
        src/http/upload.cpp
        src/http/scripting.cpp
        src/http/planning.cpp
        src/http/batch.cpp
        src/http/episodes.cpp
        src/http/projects.cpp
        src/http/llm_info.cpp
        src/http/config_api.cpp
        src/http/reset.cpp
        src/http/run.cpp
        src/http/voices.cpp
    )
    # Crow 的头目录：ws.cpp 要 <crow/websocket.h>。Crow 是纯头文件库，
    # 加个包含路径就够，不用链它的 app（测试里只碰 Hub 那个纯数据结构）。
    target_include_directories(changji_tests PRIVATE src "${ASIO_INCLUDE_DIR}"
                               "${crow_SOURCE_DIR}/include")
    target_link_libraries(changji_tests PRIVATE
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus)
    # jobs.cpp 起线程。MinGW/Linux 上不显式链 pthread 会在运行期崩，
    # 而不是链接期报错——症状是 std::thread 构造时抛 system_error。
    if(NOT MSVC)
        find_package(Threads REQUIRED)
        target_link_libraries(changji_tests PRIVATE Threads::Threads)
    endif()
    if(WIN32)
        target_link_libraries(changji_tests PRIVATE shell32)
    endif()
    target_include_directories(changji_tests PRIVATE "${doctest_SOURCE_DIR}")

    # 一个只会把参数打回来的小程序，给 proc::run 的引用规则当靶子。
    # **必须是真的 .exe**：Windows 跑 .bat 一定经过 cmd.exe，
    # 那测出来的是 cmd 的分隔规则，不是 CreateProcessW 的。
    add_executable(changji_argecho tests/tools/argecho.cpp)
    target_compile_definitions(changji_tests PRIVATE
        CHANGJI_ARGECHO="$<TARGET_FILE:changji_argecho>")
    add_dependencies(changji_tests changji_argecho)
    target_compile_features(changji_tests PRIVATE cxx_std_20)

    # 对拍语料的位置。测试要读 Python 写出来的真实项目文件。
    target_compile_definitions(changji_tests PRIVATE
        CHANGJI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")

    if(MSVC)
        target_compile_options(changji_tests PRIVATE /utf-8 /Zc:__cplusplus /W4)
    else()
        target_compile_options(changji_tests PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME unit COMMAND changji_tests)

    # ---- 对拍程序 ----
    #
    # 方案第六节。**这个目标补的是单元测试够不着的那一层**：
    # 那边直接调 http/*.cpp 里的纯函数，绕过了路由注册、查询参数解析、
    # 状态码和 Content-Type。阶段 3 就是这么丢过四个接口的——
    # 函数写好了、测试也过了，但根本没挂上路由。
    #
    # 不进 add_test：它要连一个活着的后端，不是能在 ctest 里无条件跑的东西。
    add_executable(changji_compat
        tests/compat/main.cpp
        tests/compat/diff.cpp
    )
    target_include_directories(changji_compat PRIVATE src tests/compat)
    target_include_directories(changji_compat BEFORE PRIVATE "${httplib_SOURCE_DIR}")
    target_link_libraries(changji_compat PRIVATE
        nlohmann_json::nlohmann_json httplib::httplib)
    target_compile_features(changji_compat PRIVATE cxx_std_20)
    # 数据层对拍要真的读写项目文件，所以要把 models 那几个编进来。
    target_sources(changji_compat PRIVATE
        src/util/paths.cpp
        src/util/text.cpp
        src/models/shot.cpp
        src/models/character.cpp
        src/models/project.cpp
        src/models/hardware.cpp
        src/util/proc.cpp
        src/net/ws_client.cpp
        # WebSocket 层要连 /ws。协议那部分是纯函数，传输直接用 asio。
    )
    target_include_directories(changji_compat PRIVATE "${ASIO_INCLUDE_DIR}")
    target_compile_definitions(changji_compat PRIVATE ASIO_STANDALONE)
    if(WIN32)
        target_compile_definitions(changji_compat PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
            _WIN32_WINNT=0x0A00)
        target_link_libraries(changji_compat PRIVATE ws2_32 shell32)
    endif()
    if(MSVC)
        target_compile_options(changji_compat PRIVATE /utf-8 /Zc:__cplusplus /W4)
        if(CHANGJI_STATIC_RUNTIME)
            set_property(TARGET changji_compat PROPERTY
                         MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endif()
endif()
