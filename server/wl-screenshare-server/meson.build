project(
	'wl-screenshare-server',
    'c',
	'cpp',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.54.0',
	default_options: [
		'cpp_std=c++17',
        'c_std=c11',
		'warning_level=2',
		'werror=false',
	],
)

conf_data = configuration_data()

conf_data.set('default_codec', get_option('default_codec'))
conf_data.set('default_pix_fmt', get_option('default_pixel_format'))
conf_data.set('default_container_format', get_option('default_container_format'))

version = '"@0@"'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
	git_commit = run_command([git, 'rev-parse', '--short', 'HEAD'], check: false)
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'], check: false)
	if git_commit.returncode() == 0 and git_branch.returncode() == 0
		version = '"@0@-@1@ (" __DATE__ ", branch \'@2@\')"'.format(
			meson.project_version(),
			git_commit.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif

add_project_arguments('-DWFRECORDER_VERSION=@0@'.format(version), language: 'cpp')

include_directories(['.'])

add_project_arguments(['-Wno-deprecated-declarations'], language: 'cpp')

project_sources = ['src/server.cpp', 'src/zwlr_screencopy.cpp', 'src/xdg_output.cpp', 'src/wl_registry.cpp', 'src/zwp_linux_buffer.cpp',  'src/frame-writer.cpp', 'src/main.cpp', 'src/averr.c']

wayland_client = dependency('wayland-client', version: '>=1.20')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')



libavutil = dependency('libavutil')
libavcodec = dependency('libavcodec')
libavformat = dependency('libavformat')
libavdevice = dependency('libavdevice', required: false)
libavfilter = dependency('libavfilter')
swr = dependency('libswresample')
threads = dependency('threads')
gbm = dependency('gbm')
drm = dependency('libdrm')

conf_data.set('HAVE_LIBAVDEVICE', libavdevice.found())

configure_file(input: 'config.h.in',
               output: 'config.h',
               configuration: conf_data)

subdir('proto')

dependencies = [
    wayland_client, wayland_protos,
    libavutil, libavcodec, libavformat, libavdevice, libavfilter,
    wf_protos, threads, swr, gbm, drm
]

executable('wl-screenshare-server', project_sources,
        dependencies: dependencies,
        install: true)

