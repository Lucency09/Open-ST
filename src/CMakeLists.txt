# common 是唯一允许跨目录层级依赖的公共模块，必须先于产品依赖树声明。
add_subdirectory(Common)

# launcher 目录内部按 launcher -> application -> graphics -> capture 递归声明。
add_subdirectory(Launcher)
