project('player', 'cpp', default_options: [ 'cpp_std=c++14' ])

libav = subproject('av-cpp')
libavdep = libav.get_variable('avcpp_dep')

# Player
executable('player', [
            'src/player.cpp',
            'vc/sdl.cpp',
	    'vc/gl.cpp'
          ],
	   dependencies : [ dependency('sdl2'),
			    dependency('glesv2'),
			    dependency('threads'),
			    libavdep ])
