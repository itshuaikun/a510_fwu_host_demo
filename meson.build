project('fwu_host_demo', 'c',
    default_options: ['warning_level=2','buildtype=plain']
)
add_project_arguments(['-O3', '-g0'], language: 'c')

subdir('platform_i2c_driver')

src = ['main.c']
exe = executable('fwu_host_demo', src, 
    dependencies: [platform_i2c_driver_dep],
)
