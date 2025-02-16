project('sway-freezer', 'c')

jansson = dependency('jansson')
glib = dependency('glib-2.0')
uring = dependency('liburing')

sources = [
  'freezer.c',
  'ipc-client.c',
  'pstree.c',
]

executable('sway-freezer', sources, dependencies : [jansson, glib, uring])
