# smartdns 运行时文件树（rootfs）。
#
# 只包含运行 smartdns 必需的文件：
#   - 主程序 / ui 插件（符号链接指向 /nix/store，库文件由 RPATH 解析）
#   - 配置文件
#   - webui 静态文件
#   - CA 证书、时区
# 不包含 bash / busybox / coreutils / nix / gcc。
{ lib
, runCommand
, smartdns
, smartdnsUi ? null
, opensslRuntime
, cacert
, tzdata
, src
, version
, webuiSrc ? null
, withUI ? true
, withWebUI ? webuiSrc != null
, localtime ? "Asia/Shanghai"
}:

runCommand "smartdns-rootfs-${version}" { } ''
  mkdir -p $out/usr/sbin \
           $out/usr/lib/smartdns \
           $out/usr/share/smartdns/wwwroot \
           $out/usr/share \
           $out/etc/smartdns \
           $out/etc/ssl/certs \
           $out/var/lib/smartdns \
           $out/var/log \
           $out/run

  # 主程序（glibc 动态链接）
  ln -s ${smartdns}/bin/smartdns $out/usr/sbin/smartdns

  # ui 插件（rust 编译），smartdns 会在 /usr/lib/smartdns 下查找 smartdns_ui.so
  ${lib.optionalString (withUI && smartdnsUi != null) ''
    ln -s ${smartdnsUi}/lib/smartdns_ui.so $out/usr/lib/smartdns/smartdns_ui.so
  ''}

  # 裁剪后的 openssl 运行时组件（主程序通过 RPATH 使用，这里也暴露一份便于排查）
  mkdir -p $out/usr/lib/smartdns-runtime
  ln -s ${opensslRuntime}/lib/libssl.so.3     $out/usr/lib/smartdns-runtime/libssl.so.3
  ln -s ${opensslRuntime}/lib/libcrypto.so.3  $out/usr/lib/smartdns-runtime/libcrypto.so.3

  # 配置文件
  cp -r ${src}/etc/smartdns/. $out/etc/smartdns/

  # webui 静态文件
  ${lib.optionalString (withWebUI && webuiSrc != null) ''
    cp -r ${webuiSrc}/. $out/usr/share/smartdns/wwwroot/
  ''}

  # 启用 ui 插件（打包镜像默认可用）
  ${lib.optionalString (withUI && smartdnsUi != null) ''
    cat >> $out/etc/smartdns/smartdns.conf <<'CONF'

# ---- smartdns-ui plugin (added by nix packaging) ----
plugin smartdns_ui.so
smartdns-ui.www-root /usr/share/smartdns/wwwroot
smartdns-ui.ip http://[::]:6080
data-dir /var/lib/smartdns
CONF
  ''}

  # CA 证书：DoT/DoH 校验上游证书时使用
  ln -s ${cacert}/etc/ssl/certs/ca-bundle.crt $out/etc/ssl/certs/ca-bundle.crt
  ln -s ${cacert}/etc/ssl/certs/ca-bundle.crt $out/etc/ssl/certs/ca-certificates.crt

  # 时区数据（保留 zoneinfo，但不安装完整的 locale archive）
  ln -s ${tzdata}/share/zoneinfo $out/usr/share/zoneinfo
  ln -s /usr/share/zoneinfo/${localtime} $out/etc/localtime

  # glibc 需要的最小 NSS/passwd 文件（配置项 user 会调用 getpwnam()）
  printf 'root:x:0:0:root:/root:/bin/nologin\nnobody:x:65534:65534:nobody:/:/bin/nologin\n' > $out/etc/passwd
  printf 'root:x:0:\nnogroup:x:65534:\n' > $out/etc/group
  printf 'hosts: files dns\n' > $out/etc/nsswitch.conf
''
