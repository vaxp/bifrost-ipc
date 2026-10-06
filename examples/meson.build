daemon_writer = executable('daemon_writer', 'daemon_writer.c', dependencies: bifrost_dep)
de_reader = executable('de_reader', 'de_reader.c', dependencies: bifrost_dep)
pool_director = executable('pool_director', 'pool_director.c', dependencies: bifrost_dep)

sysmon_daemon = executable('sysmon_daemon', 'sysmon_daemon.c', dependencies: bifrost_dep, c_args: ultra_flags)
sysmon_client = executable('sysmon_client', 'sysmon_client.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)

fractal_server = executable('fractal_server', 'fractal_server.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)
fractal_client = executable('fractal_client', 'fractal_client.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)

video_stream_server = executable('video_stream_server', 'video_stream_server.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)
video_stream_client = executable('video_stream_client', 'video_stream_client.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)

sysmon_daemon_bifrost = executable('sysmon_daemon_bifrost', 'sysmon_daemon_bifrost.c', dependencies: bifrost_dep, c_args: ultra_flags)
sysmon_client_bifrost = executable('sysmon_client_bifrost', 'sysmon_client_bifrost.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)

missile_simulator = executable('missile_simulator', 'missile_simulator.c', dependencies: [bifrost_dep, m_dep, threads_dep], c_args: ultra_flags)
radar_tracker = executable('radar_tracker', 'radar_tracker.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)

hft_exchange = executable('hft_exchange', 'hft_exchange.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)
hft_trading_engine = executable('hft_trading_engine', 'hft_trading_engine.c', dependencies: [bifrost_dep, m_dep], c_args: ultra_flags)

bifrost_xdp_sender = executable('bifrost_xdp_sender', 'bifrost_xdp_sender.c', dependencies: [bifrost_dep, threads_dep], c_args: ultra_flags)
bifrost_xdp_receiver = executable('bifrost_xdp_receiver', 'bifrost_xdp_receiver.c', dependencies: [bifrost_dep, threads_dep, m_dep], c_args: ultra_flags)

bifrost_gateway_example = executable('bifrost_gateway_example', 'bifrost_gateway_example.c', dependencies: [bifrost_dep, threads_dep], c_args: ultra_flags)
bifrost_e2e_demo = executable('bifrost_e2e_demo', 'bifrost_e2e_demo.c', dependencies: [bifrost_dep, threads_dep], c_args: ultra_flags)

cmd_service_writer = executable('cmd_service_writer', 'cmd_service_writer.c', dependencies: bifrost_dep, c_args: ultra_flags)
cmd_client_reader = executable('cmd_client_reader', 'cmd_client_reader.c', dependencies: bifrost_dep, c_args: ultra_flags)
