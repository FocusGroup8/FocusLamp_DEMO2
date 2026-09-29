FROM python:3.9-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean

COPY requirements.txt .

RUN pip install --no-cache-dir -i https://pypi.tuna.tsinghua.edu.cn/simple -r requirements.txt \
    && rm -rf /root/.cache/pip \
    && apt-get purge -y --auto-remove build-essential cmake

COPY . .

EXPOSE 8005

CMD ["sh", "-c", "gunicorn --workers 1 --threads 8 --timeout 120 --bind 0.0.0.0:${PORT:-9005} server:app"]
