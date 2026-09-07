cmake_minimum_required(VERSION 3.20)
project(changji CXX)

# C++20。Crow 需要 17 起步，20 的 std::format 和 designated initializer
# 在移植 pydantic 那批结构体时省很多事。
set(CMAKE_CXX_STANDARD 20)
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
    src/http/ws.cpp
    src/http/server.cpp
)

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
        # 源码是 UTF-8，中文注释和中文字符串字面量都靠它
        target_compile_options(changji PRIVATE /utf-8 /W4)
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
