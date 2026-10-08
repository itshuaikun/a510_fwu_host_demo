backend = get_option('i2c_backend')

subdir(backend)

platform_i2c_driver_dep = declare_dependency(
    link_with: backend_lib,
    include_directories: include_directories('.'),
)
