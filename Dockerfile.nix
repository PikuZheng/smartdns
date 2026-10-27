# syntax=docker/dockerfile:1.7
#
# 编译时 Dockerfile：使用 Nix 构建 smartdns，并把结果打包为 nixpkgs。
#
# 产物镜像布局：
#   /nix/store/...   smartdns 的运行时闭包（由 nix 计算，只含必要的 store 路径）
#   /release/        nixpkgs 运行时文件树（主程序 / 插件 / 配置 / wwwroot / CA / 时区）
#   /nixpkgs/        闭包清单，便于核对
#
# 说明：
#   * 主程序为 glibc 动态链接编译（不是 static，也不是 musl）
#   * smartdns-ui 插件由 rust 编译，产出 smartdns_ui.so
#   * 保留 CA certificates 与 timezone，但只保留必要的 openssl 组件、不安装完整的 locale archive
#
# 用法：
#   docker buildx build -f Dockerfile.nix -t smartdns:build .
#   docker buildx build -f Dockerfile.nix.runtime --build-arg BUILD_IMAGE=smartdns:build -t smartdns:runtime .
#
# 注意：nix 是原生构建（构建机架构 = 目标架构）。多架构镜像请在对应架构的机器上分别
# 构建，或在 package/nix/default.nix 里传入 crossSystem 做交叉编译。
#
ARG NIX_IMAGE=nixos/nix:2.32.8
ARG NODE_IMAGE=node:lts-alpine
ARG RUST_IMAGE=rust:1-alpine

# ============================================================
# 1) webui 静态文件（与 deploy-docker-ui.yml 的 build-nextjs 步骤一致）
# ============================================================
FROM ${NODE_IMAGE} AS webui
ARG WEBUI_REPO=https://github.com/PikuZheng/smartdns-webui.git
ARG WEBUI_REF=
RUN apk add --no-cache git
RUN git clone --depth 1 ${WEBUI_REF:+--branch $WEBUI_REF} "${WEBUI_REPO}" /webui
WORKDIR /webui
RUN npm install && NODE_ENV=production npm run build --no-analytics

# ============================================================
# 2) 生成 Cargo.lock
#    nix 构建在沙箱内无法联网，必须提前把依赖版本固化下来
# ============================================================
FROM ${RUST_IMAGE} AS cargo-lock
COPY plugin/smartdns-ui/ /work/plugin/smartdns-ui/
WORKDIR /work/plugin/smartdns-ui
RUN cargo generate-lockfile

# ============================================================
# 3) Nix 构建
# ============================================================
FROM ${NIX_IMAGE} AS builder
USER root
ENV HOME=/root
# 可用 --build-arg NIXPKGS_URL=https://github.com/NixOS/nixpkgs/archive/refs/tags/25.11.tar.gz 固定版本
ARG NIXPKGS_URL=https://github.com/NixOS/nixpkgs/archive/refs/heads/nixos-unstable.tar.gz
ARG SMARTDNS_VERSION=1.2026.10.05
ARG WITH_UI=true
ARG WITH_ZLIB=true

# 容器内没有 user namespace，需要关闭 nix sandbox
RUN mkdir -p /etc/nix \
 && { echo 'sandbox = false'; \
      echo 'build-users-group ='; \
      echo 'experimental-features = nix-command flakes'; \
      echo 'substituters = https://cache.nixos.org/'; \
      echo 'trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY='; } >> /etc/nix/nix.conf

# 拉取 nixpkgs
RUN mkdir -p /nixpkgs \
 && curl -fsSL "${NIXPKGS_URL}" | tar -xz -C /nixpkgs --strip-components=1

COPY . /src/
COPY --from=cargo-lock /work/plugin/smartdns-ui/Cargo.lock /src/plugin/smartdns-ui/Cargo.lock
COPY --from=webui /webui/out /webui-out

RUN set -eu; \
    export NIX_PATH="nixpkgs=/nixpkgs"; \
    nix_args="--show-trace \
      --arg src /src \
      --arg webuiSrc /webui-out \
      --arg cargoLockFile /src/plugin/smartdns-ui/Cargo.lock \
      --argstr version ${SMARTDNS_VERSION} \
      --arg withUI ${WITH_UI} \
      --arg withWebUI true \
      --arg withZlib ${WITH_ZLIB}"; \
    rootfs=$(nix-build /src/package/nix/default.nix $nix_args -A rootfs --no-out-link); \
    closure=$(nix-build /src/package/nix/default.nix $nix_args -A closureInfo --no-out-link); \
    mkdir -p /out/nix/store /out/root /out/nixpkgs; \
    cp -a "$rootfs/." /out/root/; \
    cp "$closure/store-paths" /out/nixpkgs/store-paths; \
    for p in $(cat "$closure/store-paths"); do cp -a "$p" /out/nix/store/; done; \
    echo "=== runtime closure ==="; cat /out/nixpkgs/store-paths

# ============================================================
# 4) 产物镜像（nixpkgs 闭包 + 运行时文件树）
#    这里的 alpine 只用于检查产物，运行时镜像不会带上它的 busybox/apk
# ============================================================
FROM alpine:3.23 AS release
COPY --from=builder /out/nix /nix
COPY --from=builder /out/root/ /release/
COPY --from=builder /out/nixpkgs/ /nixpkgs/
LABEL org.opencontainers.image.title="smartdns (nix build artifacts)" \
      org.opencontainers.image.description="smartdns nixpkgs: /nix/store closure + /release runtime rootfs" \
      org.opencontainers.image.source="https://github.com/pymumu/smartdns"
CMD ["/release/usr/sbin/smartdns", "-f", "-x", "-p -"]
