# 只保留 smartdns 运行时真正需要的 openssl 组件。
#
# nixpkgs 的 openssl 输出里包含 openssl CLI、头文件、静态库、man、perl 脚本等，
# 这些都不会被 smartdns 使用。这里只拷贝 libcrypto.so.3 / libssl.so.3，
# 并用 remove-references-to 断开与完整 openssl 的引用关系，
# 这样运行时闭包里就不会出现整个 openssl。
#
# 注意：这里只做“裁剪”，不重新编译，所以 ABI 与 nixpkgs 的 openssl 完全一致。
{ lib
, runCommand
, patchelf
, removeReferencesTo
, openssl
, zlib
# legacy provider（MD4/RC4 等老算法）。smartdns 的 DoT/DoH 不需要，默认关闭。
, legacyProvider ? false
}:

runCommand "openssl-runtime-${openssl.version}" {
  nativeBuildInputs = [ patchelf removeReferencesTo ];

  passthru = {
    inherit (openssl) version;
    untrimmed = openssl;
  };

  meta = openssl.meta // {
    description = "Minimal OpenSSL runtime (libcrypto/libssl only) for smartdns";
  };
} ''
  mkdir -p $out/lib
  install -m 0755 ${openssl.out}/lib/libcrypto.so.3 $out/lib/
  install -m 0755 ${openssl.out}/lib/libssl.so.3     $out/lib/

  ${lib.optionalString legacyProvider ''
    if [ -d ${openssl.out}/lib/ossl-modules ]; then
      mkdir -p $out/lib/ossl-modules
      cp -r ${openssl.out}/lib/ossl-modules/. $out/lib/ossl-modules/
      chmod -R u+w $out/lib/ossl-modules
    fi
  ''}

  # 断开对完整 openssl 的引用，避免 openssl 的 bin/include/man 进入运行时闭包
  remove-references-to -t ${openssl.out} \
    $out/lib/libcrypto.so.3 $out/lib/libssl.so.3

  # libcrypto 依赖 zlib；libssl 依赖 libcrypto（同目录，用 $ORIGIN 解析）
  patchelf --set-rpath '${zlib}/lib'              $out/lib/libcrypto.so.3
  patchelf --set-rpath '${zlib}/lib:$ORIGIN'      $out/lib/libssl.so.3
''
