project('fwu_host_demo', 'c',
    default_options: ['warning_level=2','buildtype=release']
)

subdir('platform_i2c_driver')

src = ['main.c']
exe = executable('fwu_host_demo', src, 
    dependencies: [platform_i2c_driver_dep],
)

i2c_cli_src = ['i2c_cli.c']
i2c_cli_exe = executable('i2c_cli', i2c_cli_src,
    dependencies: [platform_i2c_driver_dep],
)

stage_probe_src = ['stage_probe.c']
stage_probe_exe = executable('stage_probe', stage_probe_src,
    dependencies: [platform_i2c_driver_dep],
)
