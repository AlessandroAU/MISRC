project('MISRC', 'c',
        default_options: ['c_std=c11', 'buildtype=release', 'b_lto=true'],
        meson_version: '>=0.54')


version_target = vcs_tag(command: ['git-version.sh'], fallback: 'unknown_version', input: 'version.h.in', output: 'version.h')

warnings = [
  '-Wall',
  '-Wextra',
  '-Wshadow',
]

cflags = [
  warnings
]

ldflags = [
]

ldflags_capture = [
]

sources_extract = [
  'misrc_extract.c',
  'extract.c',
  version_target
]

sources_capture = [
  'misrc_capture.c',
  'extract.c',
  'ringbuffer.c',
  version_target
]

debug_build = get_option('buildtype').startswith('debug')

host_cpu_family = host_machine.cpu_family()

host_system = host_machine.system()

if host_cpu_family == 'x86_64'
  nasm_flags = []
  if host_system == 'windows' or host_system == 'cygwin'
    if host_cpu_family == 'x86'
      cflags += '-mstackrealign'
      ldflags += '-Wl,--kill-at'
      nasm_flags += '-DPREFIX'
    endif
    nasm_flags += ['-f', 'win64']
  elif host_system == 'linux' or host_system == 'bsd' # The BSDs are close enough, right?
    if debug_build
      nasm_flags += '-gdwarf'
    endif
    nasm_flags += ['-f', 'elf64']
  elif host_system == 'darwin'
    if debug_build
      nasm_flags += '-gdwarf'
    endif
    nasm_flags += ['--prefix', '_', '-f', 'macho64']
    cflags += ['-DPREFIX']
  else
    error('Unknown host system "@0@".'.format(host_system))
  endif

  nasm_sources = [
    'extract.asm',
  ]

  nasm = find_program('nasm')

  outputname = '@BASENAME@.o'
  if host_system == 'windows'
    outputname = '@BASENAME@.obj'
  endif

  nasm_gen = generator(nasm,
                       output: outputname,
                       arguments: nasm_flags + ['@INPUT@', '-o', '@OUTPUT@'])

  nasm_genb = nasm_gen.process(nasm_sources)
  sources_extract += nasm_genb
  sources_capture +=nasm_genb
endif


deps = [ dependency('hsdaoh') ]

flac_dep =  dependency('flac', required : false)
if flac_dep.found()
  deps += [ flac_dep ]
  cflags += ['-DLIBFLAC_ENABLED=1']
  if host_system == 'windows'
    cflags += ['-DFLAC__NO_DLL=1']
  endif
  message('FLAC found, building with FLAC support')
else
  cflags += ['-DLIBFLAC_ENABLED=0']
  message('FLAC not found, building without FLAC support')
endif

soxr_dep =  dependency('soxr', required : false)
if soxr_dep.found()
  deps += [ soxr_dep ]
  cflags += ['-DLIBSOXR_ENABLED=1']
  message('libsoxr found, building with resample support')
else
  cflags += ['-DLIBSOXR_ENABLED=0']
  message('libsoxr not found, building without resample support')
endif


if host_system == 'windows' or host_system == 'cygwin'
  if meson.get_compiler('c').get_id() != 'gcc' and meson.get_compiler('c').get_id() != 'clang'
    sources_extract += [ 'getopt/getopt.c' ]
    sources_capture += [ 'getopt/getopt.c' ]
  endif
  cflags += [ '-DNTDDI_VERSION=NTDDI_WIN10_RS4', '-D_WIN32_WINNT=_WIN32_WINNT_WIN10' ]
  ldflags_capture += [ '-lmf', '-lmfplat', '-lmfuuid', '-lmfreadwrite', '-lole32', '-lonecore', '-static' ]
  sources_capture += 'simple_capture/simple_capture_mediafoundation.c'
  if host_cpu_family == 'aarch64'
    ldflags_capture += [ '-lwinpthread' ]
  endif
elif host_system == 'linux'
  sources_capture += 'simple_capture/simple_capture_v4l2.c'
  ldflags_capture += [ '-lm' ]
elif host_system == 'darwin'
  add_languages('objc')
  ldflags_capture += ['-Wl,-framework,Cocoa', '-Wl,-framework,AVFoundation', '-Wl,-framework,CoreMedia', '-Wl,-framework,CoreVideo', '-Wl,-framework,IOKit', '-Wl,-framework,CoreFoundation', '-Wl,-framework,Security']
  sources_capture += 'simple_capture/simple_capture_avfoundation.m'
endif

executable('misrc_extract',
              sources_extract,
              dependencies: [],
              link_args: ldflags,
              c_args: cflags,
              install: true)

executable('misrc_capture',
              sources_capture,
              dependencies: deps,
              link_args: ldflags + ldflags_capture,
              c_args: cflags,
              install: true)

# GUI application (requires raylib)
raylib_dep = dependency('raylib', required: false)

if raylib_dep.found()
  sources_gui = [
    'misrc_gui/misrc_gui.c',
    'misrc_gui/gui_ui.c',
    'misrc_gui/gui_render.c',
    'misrc_gui/gui_oscilloscope.c',
    'misrc_gui/gui_phosphor.c',
    'misrc_gui/gui_dropdown.c',
    'misrc_gui/gui_trigger.c',
    'misrc_gui/gui_capture.c',
    'misrc_gui/gui_record.c',
    'misrc_gui/gui_extract.c',
    'misrc_gui/clay_renderer_raylib.c',
    'extract.c',
    'ringbuffer.c',
    version_target
  ]

  if host_cpu_family == 'x86_64'
    sources_gui += nasm_genb
  endif

  gui_deps = deps + [ raylib_dep ]

  gui_cflags = cflags

  # GUI doesn't need all the ldflags_capture (e.g., onecore conflicts with raylib's CloseWindow)
  gui_ldflags = ldflags

  if host_system == 'windows' or host_system == 'cygwin'
    gui_ldflags += ['-static']
    # gui_ldflags += ['-mwindows']  # Hide console window - disabled for debugging
    gui_ldflags += ['-lopengl32', '-lgdi32', '-lwinmm']
    # Need mincore for VirtualAlloc2/MapViewOfFile3 used by ringbuffer
    gui_ldflags += ['-lmincore']
  elif host_system == 'linux'
    gui_ldflags += ['-lGL', '-lm', '-lpthread', '-ldl', '-lrt', '-lX11']
  elif host_system == 'darwin'
    gui_ldflags += ['-Wl,-framework,OpenGL', '-Wl,-framework,Cocoa', '-Wl,-framework,IOKit', '-Wl,-framework,CoreAudio', '-Wl,-framework,CoreVideo']
  endif

  executable('misrc_gui',
              sources_gui,
              dependencies: gui_deps,
              link_args: gui_ldflags,
              c_args: gui_cflags,
              install: true)

  message('raylib found, building GUI application')
else
  message('raylib not found, skipping GUI application')
endif