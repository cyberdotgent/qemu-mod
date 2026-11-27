# lwIP, the small TCP/IP stack, for QEMU (vendored: see README.qemu)
project('lwip', 'c', version: '2.2.1', license: 'BSD-3-Clause',
        meson_version: '>=0.63.0',
        default_options: ['warning_level=1', 'werror=false'])

lwip_inc = include_directories('port', 'src/include')

lwip_sources = files(
  'src/core/def.c',
  'src/core/inet_chksum.c',
  'src/core/init.c',
  'src/core/ip.c',
  'src/core/mem.c',
  'src/core/memp.c',
  'src/core/netif.c',
  'src/core/pbuf.c',
  'src/core/raw.c',
  'src/core/stats.c',
  'src/core/sys.c',
  'src/core/tcp.c',
  'src/core/tcp_in.c',
  'src/core/tcp_out.c',
  'src/core/timeouts.c',
  'src/core/udp.c',
  'src/core/ipv4/etharp.c',
  'src/core/ipv4/icmp.c',
  'src/core/ipv4/ip4.c',
  'src/core/ipv4/ip4_addr.c',
  'src/core/ipv4/ip4_frag.c',
  'src/netif/ethernet.c',
)

cc = meson.get_compiler('c')
lwip_cflags = cc.get_supported_arguments('-fno-strict-aliasing',
                                         '-Wno-address',
                                         '-Wno-unused-parameter')

lwip_lib = static_library('lwip', lwip_sources,
                          include_directories: lwip_inc,
                          c_args: lwip_cflags,
                          pic: true,
                          build_by_default: false,
                          install: false)

lwip_dep = declare_dependency(link_with: lwip_lib,
                              include_directories: lwip_inc)
meson.override_dependency('lwip', lwip_dep)
