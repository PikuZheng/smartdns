# smartdns nixpkgs 打包入口。
#
# 用法（在编译镜像内）:
#   nix-build ./package/nix/default.nix -A rootfs --no-out-link \
#     --arg src /src \
#     --arg webuiSrc /webui-out \
#     --arg cargoLockFile /src/plugin/smartdns-ui/Cargo.lock \
#     --argstr version 1.2026.10.05
#
# 暴露的属性:
#   smartdns       主程序（glibc 动态链接）
#   smartdnsUi     smartdns-ui 插件（rust 编译，smartdns_ui.so）
#   opensslRuntime 裁剪后的 openssl 运行时（仅 libssl/libcrypto）
#   rootfs         运行时文件树（配置 / wwwroot / CA / 时区 / 符号链接）
#   closureInfo    运行时 /nix/store 闭包清单（$out/store-paths）
#
# 参数:
#   pkgs / nixpkgs  nixpkgs 来源，默认取 NIX_PATH 里的 <nixpkgs>
#   src             仓库根目录
#   webuiSrc        webui 构建产物目录（out/）
#   cargoLockFile   smartdns-ui 的 Cargo.lock（nix 沙箱内无法联网生成）
#   version         版本号
#   withUI          是否打包 ui 插件
#   withWebUI       是否打包 webui 静态文件
#   withZlib        主程序是否启用 zlib
#
{ pkgs ? null
, nixpkgs ? <nixpkgs>
, system ? builtins.currentSystem
, src ? ./../..
, webuiSrc ? null
, cargoLockFile ? null
, version ? "unknown"
, withUI ? true
, withWebUI ? webuiSrc != null
, withZlib ? true
}:

let
  pkgs' = if pkgs != null then pkgs else import nixpkgs { inherit system; };
  lib = pkgs'.lib;

  inherit (pkgs') cacert tzdata callPackage closureInfo;

  # 构建在沙箱里进行，无法直接访问 /webui-out、Cargo.lock 这类 store 之外的路径，
  # 先用 builtins.path 把它们导入 nix store。
  webui = if webuiSrc != null then
    builtins.path { path = builtins.toString webuiSrc; name = "smartdns-webui"; }
  else null;
  lock = if cargoLockFile != null then
    builtins.path { path = builtins.toString cargoLockFile; name = "Cargo.lock"; }
  else null;

  # 只保留运行时真正需要的 openssl 组件
  opensslRuntime = callPackage ./openssl-min.nix { };

  smartdns = callPackage ./smartdns.nix {
    inherit src version withZlib opensslRuntime;
  };

  smartdnsUi = if withUI then callPackage ./smartdns-ui.nix {
    inherit src version;
    cargoLockFile = lock;
  } else null;

  rootfs = callPackage ./runtime.nix {
    inherit src smartdns smartdnsUi opensslRuntime cacert tzdata version;
    inherit withUI withWebUI;
    webuiSrc = webui;
  };

  # 运行时需要的 /nix/store 路径清单（供 docker 镜像 COPY 使用）
  # rootfs 里的符号链接已经引用了 smartdns / 插件 / cacert / tzdata，
  # webui 文件是直接拷贝进 rootfs 的，不再需要单独的 store 路径。
  closure = closureInfo {
    rootPaths = [ rootfs smartdns ] ++ lib.optional (smartdnsUi != null) smartdnsUi;
  };
in
{
  inherit smartdns smartdnsUi opensslRuntime rootfs;
  closureInfo = closure;

  # 一次性构建全部产物
  all = [ smartdns rootfs closure ] ++ lib.optional (smartdnsUi != null) smartdnsUi;
}
