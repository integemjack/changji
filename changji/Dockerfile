# 场记。一个二进制，接口、编排、推理全在进程内。
#
# 原来这里是 `python:3.12-slim` + `pip install .`，旁边另有一份
# Dockerfile.cpp。阶段 8 删掉 Python 引擎之后两份并成了这一份，
# 镜像小一个数量级——Python 那版光解释器加依赖就 400 MB 起步，
# 这边运行时只有 ffmpeg、字体和一个可执行文件。
#
# **CHANGJI_SD 和 CHANGJI_LLAMA 都开着。** 早先这个镜像是两样都关的，
# 因为推理交给 comfyui 服务。ComfyUI 那条路 2026-09-10 拆了，现在
# `[models].engine` 只认 `sd`、`[tts].backend` 默认 `local`——两样都关
# 编出来的二进制配置校验都过不去，起来就报错。
#
# 代价是构建慢（sd.cpp + llama.cpp + 两份 ggml）。CPU 后端，没有 CUDA：
# 容器里做推理本来就该把显卡的事留给宿主机上的二进制。

# ---- 编译 ----
FROM debian:bookworm-slim AS build

# 走 http 不走 https：这台机器上有 TLS 拦截（容器不认那个签发者，
# 报 "certificate issuer is unknown"，IP 落在 198.18.0.0/15 那段），
# https 的 apt 源在容器里直接握不上手。apt 自己有 GPG 签名校验，不靠 TLS。
ARG APT_MIRROR=mirrors.tuna.tsinghua.edu.cn
RUN if [ -n "$APT_MIRROR" ]; then \
        for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.sources; do \
            [ -f "$f" ] && sed -i \
                -e "s|http://deb.debian.org|http://$APT_MIRROR|g" \
                -e "s|http://security.debian.org|http://$APT_MIRROR|g" "$f" || true; \
        done; \
    fi

# python3 是 CHANGJI_LLAMA=ON 要的：llama.cpp 拉下来要就地打 leejet 的
# 扩展补丁（patches/apply_to_llamacpp.py），CMakeLists 里
# find_package(Python3 REQUIRED) 找不到解释器会在配置期就停下。
#
# libssl-dev 是 CHANGJI_SSL=ON 要的：httplib 发 https 靠它，而 CMakeLists
# **只认静态库**（.a）——发出去的二进制不能链构建机上的 libssl.so。
# Debian 的 libssl-dev 里带 libssl.a / libcrypto.a，装上就够，
# 不用像 CI 那样自己编一份。
RUN apt-get update && apt-get install -y --no-install-recommends \
        -o Acquire::Retries=5 \
        build-essential cmake ninja-build git ca-certificates python3 \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY cpp/ ./cpp/

# FetchContent 要联网拉 json / toml++ / httplib / asio / Crow。
# 关掉测试：doctest 也是 FetchContent 拉的，镜像里不跑单元测试。
RUN cmake -S cpp -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCHANGJI_SD=ON \
        -DCHANGJI_LLAMA=ON \
        -DCHANGJI_BUILD_TESTS=OFF \
    && cmake --build build --target changji

# ---- 运行 ----
FROM debian:bookworm-slim

ARG APT_MIRROR=mirrors.tuna.tsinghua.edu.cn
# **一个环境变量都不用来锁配置项。** 环境变量优先级最高，会永久盖住
# 用户在界面上改的值，那样"保存"就是假的。这里只设两样跟配置无关的：
# 区域设置（不设的话容器里所有中文变问号）和项目库位置（由挂载决定，
# 不是给人在界面上改的）。
#
# 早先这里锁过 CHANGJI_MODELS_ENGINE=comfy，因为那时镜像是 SD=OFF 编的、
# 里面根本没有 sd.cpp。现在两样都编进来了，锁没有意义也不该锁。
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    CHANGJI_WORKSPACE=/data/projects

RUN if [ -n "$APT_MIRROR" ]; then \
        for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.sources; do \
            [ -f "$f" ] && sed -i \
                -e "s|http://deb.debian.org|http://$APT_MIRROR|g" \
                -e "s|http://security.debian.org|http://$APT_MIRROR|g" "$f" || true; \
        done; \
    fi

# ffmpeg 是装配环节的硬依赖；思源黑体给中文字幕烧录，缺了渲染成方框。
# curl 只为 HEALTHCHECK。
RUN apt-get update && apt-get install -y --no-install-recommends \
        -o Acquire::Retries=5 \
        ffmpeg fonts-noto-cjk curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/changji /usr/local/bin/changji

# entrypoint 干的事是"配置文件不存在就播一份，然后 exec 后面的命令"。
# 播的位置是 /root/.config/changji/config.toml——验过 C++ 在 Linux 上找的
# 正是这个路径（/api/connections 的 config_file 回的就是它）。
#
# 没有它的话，compose 里那几个 CHANGJI_SEED_* 全部失效：
# 引擎自己一个 SEED 都不认（grep 过，零处），播种从来就是镜像层干的事。
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh && mkdir -p /data/projects

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/api/health || exit 1

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

# 0.0.0.0 而不是默认的 127.0.0.1：容器里只听 127.0.0.1 等于谁也连不上。
CMD ["changji", "--host", "0.0.0.0", "--port", "8080"]
