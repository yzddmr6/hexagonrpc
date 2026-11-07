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

test('iobuffer', test_iobuffer)
test('hexagonfs', test_hexagonfs, args : [sample_file])
test('hexagonrpc', test_hexagonrpc)

# Substitute allocation and ioctl calls to exercise errors without a device.
test_type_sequence = executable('test_type_sequence',
  'test_type_sequence.c',
  '../libhexagonrpc/hexagonrpc.c',
  c_args : cflags,
  include_directories : include,
  link_args : ['-Wl,--wrap=malloc', '-Wl,--wrap=calloc'],
)
foreach scenario : [
  'input-first', 'input-after-other', 'input-two', 'input-empty',
  'output-plain', 'output-nested', 'output-two', 'output-empty',
  'in-out', 'ioctl-failure', 'allocation-failure',
]
  test('type-sequence-' + scenario, test_type_sequence, args : [scenario])
endforeach
