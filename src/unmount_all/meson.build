executable('unmount_all',
  ['unmount_all.c'],
  link_with: [libeinfo, librc],
  dependencies: [util_dep],
  include_directories: [incdir, einfo_incdir, rc_incdir],
  install: true,
  install_dir: rc_bindir)
