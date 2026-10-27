# smartdns 主程序（glibc 动态链接编译）。
#
# 这是一个标准的 nixpkgs 包，可以直接放进 nixpkgs 的 pkgs/by-name 里用 callPackage 调用。
{ lib
, stdenv
, patchelf
, openssl
, zlib
, opensslRuntime
, src            # 仓库根目录
, version ? "unknown"
, withZlib ? true
}:

stdenv.mkDerivation {
  pname = "smartdns";
  inherit version src;

  # 动态链接编译：不使用 STATIC，依赖 glibc + openssl + zlib
  nativeBuildInputs = [ patchelf ];
  buildInputs = [ openssl ] ++ lib.optional withZlib zlib;

  dontConfigure = true;
  enableParallelBuilding = true;

  buildPhase = ''
    runHook preBuild

    make -C src all \
      NO_GIT_VER=1 \
      VER="${version}" \
      WITH_ZLIB=${if withZlib then "1" else "0"} \
      -j $NIX_BUILD_CORES

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm0755 src/smartdns $out/bin/smartdns
    runHook postInstall
  '';

  # 只链接裁剪后的 openssl，避免把完整 openssl 拉进运行时闭包
  postFixup = ''
    patchelf --set-rpath "${lib.makeLibraryPath [ opensslRuntime zlib stdenv.cc.libc ]}" \
      $out/bin/smartdns
  '';

  meta = with lib; {
    description = "A local DNS server to obtain the fastest website IP for the best Internet experience";
    homepage = "https://github.com/pymumu/smartdns";
    license = licenses.gpl3Plus;
    mainProgram = "smartdns";
    platforms = platforms.linux;
  };
}
