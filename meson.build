project('player', 'cpp', default_options: [ 'cpp_std=c++14' ])

libav = subproject('av-cpp')
libavdep = libav.get_variable('avcpp_dep')

libvc = subproject('libvc')
libvcdep = libvc.get_variable('libvc_dep')

# Player
executable('player', [
            'src/player.cpp'
          ],
	   dependencies : [ dependency('sdl2'),
			    dependency('threads'),
			    libavdep, libvcdep ])
