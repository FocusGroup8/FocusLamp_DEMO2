FROM python:3.9-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    libgl1 \
    libglib2.0-0 \
    libsm6 \
    libxext6 \
    libxrender1 \
    libxcb1 \
    fonts-wqy-microhei \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean

COPY requirements.txt .

RUN pip install --no-cache-dir -i https://pypi.tuna.tsinghua.edu.cn/simple -r requirements.txt \
    && rm -rf /root/.cache/pip

COPY . .

EXPOSE 9010

CMD ["sh", "-c", "gunicorn --workers 1 --threads 8 --timeout 120 --bind 0.0.0.0:${PORT:-9010} server:app"]
