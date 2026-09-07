# ComfyUI for RTX 50 系列 (Blackwell) - PyTorch 2.14 + CUDA 13.0
# ComfyUI 官方要求：20 系及以上显卡需要 cu130 或更高版本的 PyTorch
FROM pytorch/pytorch:2.14.0-cuda13.0-cudnn9-runtime

# 基础镜像用系统 Python 3.12 且带 PEP 668 保护标记，容器内装包需要放开
# 直连 pypi.org 会间歇性 SSL 断流，改走清华源
ENV DEBIAN_FRONTEND=noninteractive \
    PIP_NO_CACHE_DIR=1 \
    PIP_BREAK_SYSTEM_PACKAGES=1 \
    PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple \
    PIP_EXTRA_INDEX_URL=https://mirrors.aliyun.com/pypi/simple \
    PIP_RETRIES=10 \
    PIP_TIMEOUT=60 \
    PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
        git ffmpeg libgl1 libglib2.0-0 build-essential curl aria2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app/ComfyUI

# 依赖（torch/torchvision/torchaudio 已在基础镜像中且是 cu130 版，
# 从 requirements 里排除，避免 pip 从 pypi 拉 CPU 版把它们覆盖掉）
COPY ComfyUI/requirements.txt /tmp/requirements.txt
COPY ComfyUI/manager_requirements.txt /tmp/manager_requirements.txt
RUN grep -vE '^(torch|torchvision|torchaudio)\s*$' /tmp/requirements.txt > /tmp/req.txt \
    && pip install -r /tmp/req.txt -r /tmp/manager_requirements.txt

# 自定义节点的额外依赖：写进 extra-requirements.txt 后重新 build
COPY extra-requirements.txt /tmp/extra-requirements.txt
RUN pip install -r /tmp/extra-requirements.txt

# 校验 torch 没被降级成 CPU 版或旧 CUDA 版
RUN python -c "import torch; print('torch', torch.__version__, 'cuda', torch.version.cuda); assert torch.version.cuda and torch.version.cuda.startswith('13'), 'CUDA 版本不对: ' + str(torch.version.cuda)"

EXPOSE 8188
CMD ["python", "main.py", "--listen", "0.0.0.0", "--port", "8188", "--enable-manager"]
