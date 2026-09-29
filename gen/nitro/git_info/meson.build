gen_git_info_py  = find_program('gen_git_info.py', native: true)

git_info_h = custom_target('git_info.h',
    input: fx_const_csv,
    output: 'git_info.h',
    command: [gen_git_info_py, '@INPUT@', '@OUTPUT@', meson.project_source_root(), meson.global_source_root()]
)
