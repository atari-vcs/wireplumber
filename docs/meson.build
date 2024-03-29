# Find dependencies
pymod = import('python')

python_doc = pymod.find_installation(
  'python3',
  modules: ['sphinx', 'sphinx_rtd_theme', 'breathe', 'sphinx.ext.graphviz'],
  required: get_option('doc')
)

python_gir = pymod.find_installation(
  'python3',
  modules: ['lxml'],
  required: get_option('introspection')
)

if get_option('doc').enabled() or get_option('introspection').enabled()
  doxygen_p = find_program('doxygen', version: '>= 1.9.0', required: true)
elif get_option('doc').auto() or get_option('introspection').auto()
  doxygen_p = find_program('doxygen', version: '>= 1.9.0', required: false)
else
  doxygen_p = disabler()
endif

sphinx_p = find_program('sphinx-build',
    version: '>= 2.1.0', required: get_option('doc'))
gir_p = find_program('g-ir-scanner', required: get_option('introspection'))

build_doc = python_doc.found() and doxygen_p.found() and sphinx_p.found()
build_gir = python_gir.found() and doxygen_p.found() and gir_p.found()

# Run doxygen (common for docs and g-i)

if build_doc or build_gir
  doxy_wp_conf_data = configuration_data()
  doxy_wp_conf_data.set('OUTPUT_DIR', meson.current_build_dir() / 'wp')
  doxy_wp_conf_data.set('INPUT', meson.source_root() / 'lib' / 'wp')
  doxyfile_wp = configure_file(
    input: 'Doxyfile.in',
    output: 'Doxyfile-wp',
    configuration: doxy_wp_conf_data
  )

  doxyxml_wp_depfiles = [wp_lib_sources, wp_lib_parse_headers]
  doxyxml_wp = custom_target('doxyxml_wp',
    command: [doxygen_p, doxyfile_wp],
    depend_files: doxyxml_wp_depfiles,
    output: 'wp',
    build_by_default: true,
  )
endif

# Build documentation

if build_doc
  sphinx_files = files(
    '_static'/'custom.css',
    meson.source_root()/'README.rst',
    meson.source_root()/'NEWS.rst',
  )
  subdir('rst')

  sphinx_conf_data = configuration_data()
  sphinx_conf_data.set('SRCDIR', meson.current_source_dir())
  sphinx_conf_data.set('OUTDIR', meson.current_build_dir())
  sphinx_conf_data.set('VERSION', meson.project_version())
  sphinx_conf = configure_file(
    input: 'conf.py.in',
    output: 'conf.py',
    configuration: sphinx_conf_data
  )

  custom_target('doc',
    command: [sphinx_p,
      '-q',                       # quiet
      '-E',                       # rebuild from scratch
      '-j', 'auto',               # parallel build
      '-d', '@PRIVATE_DIR@',      # doctrees dir
      '-c', '@OUTDIR@',           # conf.py dir
      '@CURRENT_SOURCE_DIR@/rst', # source dir
      '@OUTPUT@',                 # output dir
    ],
    depend_files: [
      sphinx_conf, sphinx_files,
      doxyfile_wp, doxyxml_wp_depfiles,
    ],
    depends: [doxyxml_wp],
    output: 'html',
    install: true,
    install_dir: get_option('datadir') / 'doc' / 'wireplumber',
    build_by_default: true,
  )
endif

# Build GObject introspection

if build_gir
  wp_gtkdoc_h = custom_target('wp_gtkdoc_h',
    command: [python_gir,
        '@CURRENT_SOURCE_DIR@/gen-api-gtkdoc.py',
        '-o', '@OUTPUT@',
        '@OUTDIR@/wp/xml',
    ],
    depends: [doxyxml_wp],
    depend_files: [doxyxml_wp_depfiles, 'gen-api-gtkdoc.py'],
    output: 'wp-gtkdoc.h',
    build_by_default: true,
  )

  gnome.generate_gir(wp_lib,
    dependencies: wp_dep,
    namespace: 'Wp',
    nsversion: wireplumber_api_version,
    sources: [wpenums_h, wp_lib_parse_headers, wp_gtkdoc_h],
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0'],
    install: true,
  )
endif
