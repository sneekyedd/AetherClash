# Aether Clash lobby server. Build from the repository root:
#   docker compose -f server/docker-compose.yml up -d --build
FROM python:3.12-slim
WORKDIR /app
COPY aether_server.py server/run_server.py ./
ENV PYTHONUNBUFFERED=1
EXPOSE 7777
CMD ["python", "run_server.py"]
