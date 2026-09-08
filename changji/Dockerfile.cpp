# 场记编排引擎（C++ 版）。
#
# 阶段 8 的清单第 3 条：原来的镜像是 `python:3.12-slim` + `pip install .`，
# 要换成拷一个二进制进去。收益是镜像小一个数量级——Python 那版光解释器
# 加依赖就 400 MB 起步，这边运行时只有 ffmpeg、字体和一个可执行文件。
#
# **先叫 Dockerfile.cpp，不直接盖掉 Dockerfile。** 老那份连着一份还能跑的
# Python 引擎，而 Python 要到阶段 8 才删；在那之前两个都得在。
#
# **不链 sd.cpp / llama.cpp**（CHANGJI_SD=OFF、CHANGJI_LLAMA=OFF）。
# 这一层是编排，推理交给 comfyui 服务——和 Python 那版的分工一模一样
# （那份的注释写着"只跑编排和装配，不做模型推理，所以不需要 CUDA"）。
# 要进程内推理的人在宿主机上跑二进制，不在这个镜像里。
# 所以镜像里把 [models].engine 锁成 comfy，见下面的 ENV。

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

RUN apt-get update && apt-get install -y --no-install-recommends \
        -o Acquire::Retries=5 \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY cpp/ ./cpp/

# FetchContent 要联网拉 json / toml++ / httplib / asio / Crow。
# 关掉测试：doctest 也是 FetchContent 拉的，镜像里不跑单元测试。
RUN cmake -S cpp -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCHANGJI_SD=OFF \
        -DCHANGJI_LLAMA=OFF \
        -DCHANGJI_BUILD_TESTS=OFF \
    && cmake --build build --target changji

# ---- 运行 ----
FROM debian:bookworm-slim

ARG APT_MIRROR=mirrors.tuna.tsinghua.edu.cn
# **engine 锁成 comfy，而且是用环境变量锁死的，不是播一份配置。**
#
# 平时不该这么干——环境变量优先级最高，会永久盖住界面上改的值，
# 那样"保存"就是假的（Python 那份镜像的 entrypoint 只在首次启动播种，
# 就是为了避开这一点）。
#
# 但 engine 这一项不一样：这个镜像是 CHANGJI_SD=OFF 编的，**里面根本没有
# sd.cpp**，选 "sd" 不是"另一种配置"，是"一条不存在的路"。锁死比让人在
# 界面上改成 sd、保存成功、然后跑的时候才炸要诚实。
# 锁了之后 /api/connections 的 env_locked 里会有 models_engine，
# 界面上那一项会显示成锁着的——用户看得见为什么改不动。
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    CHANGJI_WORKSPACE=/data/projects \
    CHANGJI_MODELS_ENGINE=comfy

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

# **和 Python 那版共用同一个 entrypoint，一个字都不用改。**
# 它干的事是"配置文件不存在就播一份，然后 exec 后面的命令"，
# 跟实现语言无关。播的位置是 /root/.config/changji/config.toml——
# 验过 C++ 在 Linux 上找的正是这个路径（/api/connections 的 config_file
# 回的就是它，塞进去的 base_url 也读出来了）。
#
# 没有它的话，compose 里那几个 CHANGJI_SEED_* 全部失效：
# C++ 侧一个 SEED 都不认（grep 过，零处），播种从来就是镜像层干的事。
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh && mkdir -p /data/projects

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/api/health || exit 1

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

# 0.0.0.0 而不是默认的 127.0.0.1：容器里只听 127.0.0.1 等于谁也连不上。
CMD ["changji", "--host", "0.0.0.0", "--port", "8080"]
