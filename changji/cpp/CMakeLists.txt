cmake_minimum_required(VERSION 3.20)
project(changji CXX)

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

# ---- 本体 ----

add_executable(changji
    src/main.cpp
    src/util/paths.cpp
    src/util/proc.cpp
    src/config/settings.cpp
    src/doctor/doctor.cpp
    src/models/shot.cpp
    src/models/character.cpp
    src/models/project.cpp
    src/models/hardware.cpp
    src/http/ws.cpp
    src/http/readonly.cpp
    src/http/editing.cpp
    src/http/editing_assets.cpp
    src/http/editing_batch.cpp
    src/http/media.cpp
    src/http/server.cpp
)

# 自己的代码要 C++20，但只加在自己的目标上，不传染给上游。
# 阶段 5 链 sd.cpp / llama.cpp 时这一点是必须的，见上面 CMAKE_CXX_STANDARD 那段。
target_compile_features(changji PRIVATE cxx_std_20)

target_include_directories(changji PRIVATE
    src
    "${ASIO_INCLUDE_DIR}"
)

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

if(WIN32)
    target_compile_definitions(changji PRIVATE
        _WIN32_WINNT=0x0A00   # Windows 10。asio 不给这个宏会警告并退化行为
        NOMINMAX              # windows.h 的 min/max 宏会和 std::min/max 打架
        WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE      # 路径要走宽字符，中文目录名才不会乱码
    )
    target_link_libraries(changji PRIVATE ws2_32 mswsock)
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
        tests/unit/test_shot.cpp
        tests/unit/test_character.cpp
        tests/unit/test_project.cpp
        tests/unit/test_hardware.cpp
        tests/unit/test_readonly.cpp
        tests/unit/test_editing.cpp
        tests/unit/test_editing_assets.cpp
        tests/unit/test_editing_batch.cpp
        tests/unit/test_media.cpp
        src/models/shot.cpp
        src/models/character.cpp
        src/models/project.cpp
        src/models/hardware.cpp
        src/util/paths.cpp
        src/util/proc.cpp
        src/config/settings.cpp
        src/http/readonly.cpp
        src/http/editing.cpp
        src/http/editing_assets.cpp
        src/http/editing_batch.cpp
        src/http/media.cpp
    )
    target_include_directories(changji_tests PRIVATE src "${ASIO_INCLUDE_DIR}")
    target_link_libraries(changji_tests PRIVATE
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus)
    target_include_directories(changji_tests PRIVATE "${doctest_SOURCE_DIR}")
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
endif()
