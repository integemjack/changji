// 顶栏那三个小表的数据源。真去读 /proc、问 NVML 的那些不在这儿测——
// 跑不跑得动看机器；这里测的是把文本变成数的那几个纯函数。

#include <doctest/doctest.h>

#include "util/sysstat.hpp"

using namespace changji::sysstat;

TEST_CASE("sysstat: /proc/stat 第一行，idle 含 iowait，total 是前八列") {
    const std::string text =
        "cpu  1000 20 300 5000 100 10 5 0 0 0\n"
        "cpu0 500 10 150 2500 50 5 2 0 0 0\n";
    auto t = parse_proc_stat(text);
    REQUIRE(t.has_value());
    CHECK(t->idle == 5100);
    CHECK(t->total == 1000 + 20 + 300 + 5000 + 100 + 10 + 5 + 0);
}

TEST_CASE("sysstat: /proc/stat 不是 cpu 打头或者列不够就当没读到") {
    CHECK_FALSE(parse_proc_stat("").has_value());
    CHECK_FALSE(parse_proc_stat("intr 1 2 3\n").has_value());
    CHECK_FALSE(parse_proc_stat("cpu 1 2\n").has_value());
}

TEST_CASE("sysstat: /proc/meminfo 取 MemTotal 和 MemAvailable，单位 GB") {
    const std::string text =
        "MemTotal:       67108864 kB\n"
        "MemFree:         1000000 kB\n"
        "MemAvailable:   33554432 kB\n"
        "Buffers:          123456 kB\n";
    auto m = parse_meminfo(text);
    REQUIRE(m.has_value());
    CHECK(m->first == doctest::Approx(64.0));
    CHECK(m->second == doctest::Approx(32.0));
    CHECK_FALSE(parse_meminfo("MemTotal: 1 kB\n").has_value());
}

TEST_CASE("sysstat: cgroup 上限，max 和 2^63 那种都算没设") {
    CHECK_FALSE(parse_cgroup_limit("max\n").has_value());
    CHECK_FALSE(parse_cgroup_limit("").has_value());
    CHECK_FALSE(parse_cgroup_limit("9223372036854771712\n").has_value());
    CHECK_FALSE(parse_cgroup_limit("abc").has_value());
    auto v = parse_cgroup_limit("2147483648\n");
    REQUIRE(v.has_value());
    CHECK(*v == 2147483648ULL);
}

TEST_CASE("sysstat: nvidia-smi 一张卡一行，MiB 换成 GB，[N/A] 留 -1") {
    const std::string out =
        "0, NVIDIA GeForce RTX 5090, 82, 31500, 32607\n"
        "1, NVIDIA L20, [N/A], 0, 46068\n"
        "\n";
    auto g = parse_nvidia_smi(out);
    REQUIRE(g.size() == 2);
    CHECK(g[0].index == 0);
    CHECK(g[0].name == "NVIDIA GeForce RTX 5090");
    CHECK(g[0].util_percent == 82);
    CHECK(g[0].vram_used_gb == doctest::Approx(31500.0 / 1024));
    CHECK(g[0].vram_total_gb == doctest::Approx(32607.0 / 1024));
    CHECK(g[1].index == 1);
    CHECK(g[1].util_percent == -1);
    CHECK(g[1].vram_used_gb == doctest::Approx(0));
}

TEST_CASE("sysstat: to_json 的形状，前端照这个读") {
    Load l;
    l.cpu_percent = 12.5;
    l.mem_used_gb = 8;
    l.mem_total_gb = 64;
    GpuLoad g;
    g.index = 0;
    g.name = "X";
    g.util_percent = 50;
    g.vram_used_gb = 1;
    g.vram_total_gb = 2;
    l.gpus.push_back(g);
    auto j = to_json(l);
    CHECK(j["cpu_percent"] == 12.5);
    CHECK(j["mem_total_gb"] == 64);
    REQUIRE(j["gpus"].size() == 1);
    CHECK(j["gpus"][0]["util_percent"] == 50);
    CHECK(j["gpus"][0]["name"] == "X");
}
