# syntax=docker/dockerfile:1

# --- Build stage ---
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make release

# --- Runtime stage ---
FROM debian:bookworm-slim
RUN useradd --system --no-create-home --shell /usr/sbin/nologin dantrix \
 && mkdir -p /var/www/dantrix && chown dantrix:dantrix /var/www/dantrix
COPY --from=build /src/bin/dantrix /usr/local/bin/dantrix
ENV DANTRIX_ROOT=/var/www/dantrix \
    DANTRIX_BIND=0.0.0.0 \
    DANTRIX_PORT=8080
USER dantrix
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/dantrix"]
