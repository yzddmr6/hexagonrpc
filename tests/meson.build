valgrind = find_program('valgrind', required : false)

if valgrind.found()
  add_test_setup('valgrind', exe_wrapper : valgrind)
endif

test_iobuffer = executable('test_iobuffer',
  'test_iobuffer.c',
  '../hexagonrpcd/iobuffer.c',
  c_args : cflags,
  include_directories : include,
)

sample_file = custom_target('sample_file',
  input : 'sample_file.txt',
  output : 'sample_file.txt',
  command : ['cp', '@INPUT@', '@OUTPUT@']
)
test_hexagonfs = executable('test_hexagonfs',
  'test_hexagonfs.c',
  '../hexagonrpcd/hexagonfs.c',
  '../hexagonrpcd/hexagonfs_mapped.c',
  c_args : cflags,
  include_directories : include,
)

test_hexagonrpc = executable('test_hexagonrpc',
  'test_hexagonrpc.c',
  '../libhexagonrpc/hexagonrpc.c',
  c_args : cflags,
  include_directories : include,
)

test_registry_version_contract = executable('test_registry_version_contract',
  'test_registry_version_contract.c',
  '../hexagonrpcd/rpcd_builder.c',
  '../hexagonrpcd/hexagonfs.c',
  '../hexagonrpcd/hexagonfs_mapped.c',
  '../hexagonrpcd/hexagonfs_plat_subtype_name.c',
  '../hexagonrpcd/hexagonfs_virt_dir.c',
  c_args : cflags,
  include_directories : include,
)

test_odm_sensor_config = executable('test_odm_sensor_config',
  'test_odm_sensor_config.c',
  '../hexagonrpcd/rpcd_builder.c',
  '../hexagonrpcd/hexagonfs.c',
  '../hexagonrpcd/hexagonfs_mapped.c',
  '../hexagonrpcd/hexagonfs_plat_subtype_name.c',
  '../hexagonrpcd/hexagonfs_virt_dir.c',
  c_args : cflags,
  include_directories : include,
)

test_sns_registry = executable('test_sns_registry',
  'test_sns_registry.c',
  '../hexagonrpcd/interface/adsp_listener.c',
  '../hexagonrpcd/interface/sns_registry.c',
  '../hexagonrpcd/iobuffer.c',
  '../hexagonrpcd/listener.c',
  '../hexagonrpcd/sns_registry.c',
  c_args : cflags,
  include_directories : include,
)

test_apps_std_fd = executable('test_apps_std_fd',
  'test_apps_std_fd.c',
  '../hexagonrpcd/apps_std.c',
  '../hexagonrpcd/interface/adsp_listener.c',
  '../hexagonrpcd/interface/apps_std.c',
  '../hexagonrpcd/hexagonfs.c',
  '../hexagonrpcd/hexagonfs_mapped.c',
  '../hexagonrpcd/hexagonfs_plat_subtype_name.c',
  '../hexagonrpcd/hexagonfs_virt_dir.c',
  '../hexagonrpcd/iobuffer.c',
  '../hexagonrpcd/listener.c',
  '../hexagonrpcd/rpcd_builder.c',
  c_args : cflags,
  include_directories : include,
)

test_listener_refusal = executable('test_listener_refusal',
  'test_listener_refusal.c',
  '../hexagonrpcd/apps_std.c',
  '../hexagonrpcd/interface/adsp_listener.c',
  '../hexagonrpcd/interface/apps_std.c',
  '../hexagonrpcd/hexagonfs.c',
  '../hexagonrpcd/hexagonfs_mapped.c',
  '../hexagonrpcd/hexagonfs_plat_subtype_name.c',
  '../hexagonrpcd/hexagonfs_virt_dir.c',
  '../hexagonrpcd/iobuffer.c',
  '../hexagonrpcd/listener.c',
  '../hexagonrpcd/rpcd_builder.c',
  c_args : cflags,
  include_directories : include,
)

test('iobuffer', test_iobuffer)
test('hexagonfs', test_hexagonfs, args : [sample_file])
test('hexagonrpc', test_hexagonrpc)
test('registry-version-contract', test_registry_version_contract,
  args : [meson.current_build_dir()])
test('odm-sensor-config', test_odm_sensor_config,
  args : [meson.current_build_dir() / 'odm-sensor-config'])
test('sns-registry', test_sns_registry)
test('apps-std-fd', test_apps_std_fd,
  args : [meson.current_build_dir() / 'apps-std-fd'])
test('listener-refusal', test_listener_refusal,
  args : [meson.current_build_dir() / 'listener-refusal'])
