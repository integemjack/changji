#pragma once

// cpp-httplib 的包装。**要用 httplib 的地方一律 include 这个，别直接 include。**
//
// 两件事：
//
// 一、**把警告按下去。** httplib 的头文件目录不能是 SYSTEM——sd.cpp 导出的
//     thirdparty 里有一份自己的 httplib.h，而 MSVC 的 SYSTEM 目录永远排在
//     普通 -I 之后，是 SYSTEM 就必然被盖掉（见 CMakeLists 里那段注释）。
//     不是 SYSTEM 就要自己压警告，否则 /W4 下几十条 C4244 淹掉真正的问题。
//
// 二、**留一个统一的落点。** 将来要打开 CPPHTTPLIB_OPENSSL_SUPPORT
//     或者调默认超时，改这一处就够，不用去翻每个用到它的文件。

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <httplib.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
