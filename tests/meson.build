test_basic = executable('test_basic', 'test_basic.c', dependencies: bifrost_dep)
test('BasicTest', test_basic, is_parallel: false)

test_mp = executable('test_multiprocess', 'test_multiprocess.c', dependencies: bifrost_dep)
test('MultiProcessTest', test_mp, is_parallel: false)

test_bench = executable('test_benchmark', 'benchmark.c', dependencies: [bifrost_dep, threads_dep], c_args: ultra_flags)
test('BenchmarkTest', test_bench, is_parallel: false)

test_cmd = executable('test_commands', 'test_commands.c', dependencies: [bifrost_dep, threads_dep])
test('CommandTest', test_cmd, timeout: 120, is_parallel: false)

cmd_bench = executable('cmd_bench', 'cmd_bench.c', dependencies: [bifrost_dep, threads_dep], c_args: ultra_flags)
benchmark('CommandBench', cmd_bench)

bifrost_ultra_bench = executable('bifrost_ultra_bench', 'bifrost_ultra_bench.c', dependencies: [bifrost_dep, m_dep, threads_dep], c_args: ultra_flags)
benchmark('UltraBench', bifrost_ultra_bench)
