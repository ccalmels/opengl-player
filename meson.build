project('player', 'cpp', default_options: [ 'cpp_std=c++14' ])

libav = subproject('av-cpp')
libvc = subproject('libvc')

args = []
dependencies = [ dependency('sdl2'),
		 dependency('threads'),
		 libav.get_variable('avcpp_dep'),
		 libvc.get_variable('libvc_dep') ]

cudalib = meson.get_compiler('cpp').find_library('libcuda', required : false)
if cudalib.found()
  dependencies += declare_dependency(dependencies : cudalib)
  args += '-DHAVE_CUDA'
endif

vadep = dependency('libva', required : false)
if vadep.found()
  dependencies += vadep
  args += '-DHAVE_VA'
endif

# Player
executable('player', 'src/player.cpp',
	   dependencies : dependencies, cpp_args : args)
