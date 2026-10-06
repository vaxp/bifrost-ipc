project('bifrost', 'c',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : [
    'c_std=gnu11',       
    'warning_level=2',   
    'buildtype=release'  
  ]
)

cc = meson.get_compiler('c')

add_project_arguments('-Wpedantic', language: 'c')

rt_dep = cc.find_library('rt', required: true)
m_dep = cc.find_library('m', required: false)
threads_dep = dependency('threads')

bpf_dep = cc.find_library('bpf', required: false)
xdp_dep = cc.find_library('xdp', required: false)

bifrost_deps = [rt_dep]
if bpf_dep.found() and xdp_dep.found()
  message('Found libbpf and libxdp - AF_XDP enabled')
  bifrost_deps += [bpf_dep, xdp_dep]
else
  message('libbpf/libxdp not found - AF_XDP disabled, using UDP fallback')
endif

inc_dir = include_directories('include', 'src')

bifrost_shared_sources = [
  'src/director.c', 'src/writer.c', 'src/reader.c', 
  'src/arena.c', 'src/utils.c', 'src/command.c', 'src/bifrost_netproto.c', 
  'src/bifrost_xdp.c', 'src/bifrost_gateway.c'
]

bifrost_shared = shared_library('bifrost',
  bifrost_shared_sources,
  include_directories: inc_dir,
  dependencies: bifrost_deps,
  install: true
)

bifrost_static_sources = [
  'src/director.c', 'src/writer.c', 'src/reader.c', 
  'src/arena.c', 'src/utils.c', 'src/command.c'
]

bifrost_static = static_library('bifrost_static',
  bifrost_static_sources,
  include_directories: inc_dir,
  dependencies: rt_dep,
  install: true
)

bifrost_dep = declare_dependency(
  link_with: bifrost_shared,
  include_directories: include_directories('include')
)
install_headers('include/bifrost.h', 'include/bifrost_command.h')

ultra_flags = ['-O3', '-march=native']

build_examples = true
if build_examples
  subdir('examples')
endif

build_tests = true
if build_tests
  subdir('tests')
endif