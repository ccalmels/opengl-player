project('player', 'cpp', default_options: [ 'cpp_std=c++14' ])

# Player
executable('player', [
            'src/player.cpp',
            'vc/sdl.cpp',
	    'vc/gl.cpp'
          ],
	   dependencies : [ dependency('sdl2'),
			    dependency('glesv2') ])
