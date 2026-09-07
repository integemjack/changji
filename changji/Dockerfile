# 场记编排引擎。
# 这个镜像只跑编排和装配，不做模型推理，所以不需要 CUDA，体积很小。
# 推理交给 comfyui 服务，大模型交给 ollama 服务，三者由 compose 编排。
FROM python:3.12-slim-bookworm

ENV PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    # 不设区域设置的话容器里所有中文都会变成问号。
    # 这是个中文软件，界面、错误提示、字幕全靠它。
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PYTHONIOENCODING=utf-8 \
    # 项目数据挂在这里，是容器里唯一需要持久化的路径
    CHANGJI_WORKSPACE=/data/projects

# ffmpeg 是装配环节的硬依赖，烤进镜像，用户不必自己装。
# fonts-noto-cjk 提供思源黑体，中文字幕烧录要用，缺了会渲染成方框。
# 换国内 apt 源。官方源在国内下大包会 502 或传到一半断，
# 实测在推理镜像里拉 gcc 时连续失败。境外机器把这个参数置空即可。
ARG APT_MIRROR=mirrors.tuna.tsinghua.edu.cn
RUN if [ -n "$APT_MIRROR" ]; then \
        for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.sources; do \
            [ -f "$f" ] && sed -i \
                -e "s|http://deb.debian.org|https://$APT_MIRROR|g" \
                -e "s|http://security.debian.org|https://$APT_MIRROR|g" "$f" || true; \
        done; \
    fi

RUN apt-get update && apt-get install -y --no-install-recommends \
        -o Acquire::Retries=5 \
        ffmpeg \
        fonts-noto-cjk \
        curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 先装依赖再拷源码，改代码时不必重装依赖
COPY pyproject.toml README.md ./
COPY src/ ./src/
RUN pip install --no-cache-dir .

RUN mkdir -p /data/projects

EXPOSE 8080

# 容器内互相访问用服务名，不是 localhost。
# 但这件事只在首次启动时播种进配置文件，不用环境变量常驻。
# 环境变量优先级最高，会永久盖住界面上改的地址，那样「保存」就是假的。
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/api/health || exit 1

CMD ["changji", "serve", "--host", "0.0.0.0", "--port", "8080"]
