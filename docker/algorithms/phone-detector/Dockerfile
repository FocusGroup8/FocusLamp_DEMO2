FROM python:3.9-slim

WORKDIR /app
ENV YOLO_OFFLINE=true YOLO_AUTOINSTALL=false YOLO_CONFIG_DIR=/tmp/phone-ultralytics \
    OMP_NUM_THREADS=2 MKL_NUM_THREADS=2
RUN apt-get update && apt-get install -y --no-install-recommends libgl1 libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir --upgrade pip==25.2
COPY phone-detector/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -i https://pypi.tuna.tsinghua.edu.cn/simple -r requirements.txt
COPY phone-detector/server.py phone-detector/phone_logic.py ./
COPY phone-detector/test_phone_logic.py phone-detector/test_phone_service.py ./
# Build context is algorithms/. Reuse the project's existing model asset.
COPY vlm-game-detector/yolo26s.pt ./yolo26s.pt
EXPOSE 9015
CMD ["sh", "-c", "gunicorn --workers 1 --threads 4 --timeout 120 --bind 0.0.0.0:${PORT:-9015} server:app"]
