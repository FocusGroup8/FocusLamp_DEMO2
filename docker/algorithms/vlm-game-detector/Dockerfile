FROM python:3.9-slim

# 安装系统依赖（OpenCV、MediaPipe、YOLO 需要）
RUN apt-get update && apt-get install -y --no-install-recommends \
    fonts-wqy-microhei \
    fonts-wqy-zenhei \
    libgl1 \
    libglib2.0-0 \
    libsm6 \
    libxext6 \
    libxrender1 \
    libxcb1 \
    libgles2 \
    libegl1 \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean

WORKDIR /app

# 构建上下文为 ./algorithms（见 .dockerignore），COPY 路径相对于该目录
COPY vlm-game-detector/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -i https://pypi.tuna.tsinghua.edu.cn/simple -r requirements.txt \
    && rm -rf /root/.cache/pip

COPY vlm-game-detector/server.py ./server.py
COPY vlm-game-detector/hand_landmarker.task ./hand_landmarker.task
COPY vlm-game-detector/yolo26s.pt ./yolo26s.pt
COPY _common/ ./_common/

EXPOSE 9014

CMD ["sh", "-c", "gunicorn --workers 1 --threads 8 --timeout 120 --bind 0.0.0.0:${PORT:-9014} server:app"]
