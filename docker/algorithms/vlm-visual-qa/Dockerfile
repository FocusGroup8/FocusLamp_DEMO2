FROM python:3.9-slim

# 安装中文字体（用于 PIL 渲染中文文本）
RUN apt-get update && apt-get install -y --no-install-recommends \
    fonts-wqy-microhei \
    fonts-wqy-zenhei \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 构建上下文为 ./algorithms（见 .dockerignore），COPY 路径相对于该目录
COPY vlm-visual-qa/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -i https://pypi.tuna.tsinghua.edu.cn/simple -r requirements.txt

COPY vlm-visual-qa/server.py ./server.py
COPY _common/ ./_common/

EXPOSE 9013

CMD ["sh", "-c", "gunicorn --workers 1 --threads 8 --timeout 120 --bind 0.0.0.0:${PORT:-9013} server:app"]
