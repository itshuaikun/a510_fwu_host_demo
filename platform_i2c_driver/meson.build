lib_platform_i2c_driver = static_library('platform_i2c_driver',
    sources: ['i2c_mem_rw.c'],
)

platform_i2c_driver_dep = declare_dependency(
    link_with: lib_platform_i2c_driver,
    include_directories: include_directories('.'),
)
