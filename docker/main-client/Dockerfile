FROM python:3.9-slim

WORKDIR /app

COPY requirements.txt .

RUN pip install --no-cache-dir -i https://pypi.tuna.tsinghua.edu.cn/simple -r requirements.txt \
    && rm -rf /root/.cache/pip

COPY . .

EXPOSE 9000

CMD ["python", "-u", "main.py"]
