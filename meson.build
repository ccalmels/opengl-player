project('player', 'cpp', meson_version: '>=1.1', default_options: [ 'cpp_std=c++17', 'werror=true' ])

libav = subproject('av-cpp')
libvc = subproject('libvc')

args = []
dependencies = [
  dependency('sdl2'),
  dependency('threads'),
  dependency('fmt'),
  libav.get_variable('avcpp_dep'),
  libvc.get_variable('libvc_dep'),
]

vadep = dependency('libva', required : get_option('vaapi'))
if vadep.found()
  dependencies += vadep
  args += '-DHAVE_VA'
endif

# Player
executable('player', 'src/player.cpp',
           dependencies : dependencies, cpp_args : args)
