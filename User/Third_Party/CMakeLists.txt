# =============================================================================
# Third_Party 层：别人写的代码，只读
#
#   lvgl：LVGL 10.0.0-dev 核心库。上游只保留 src 与 include 两棵树，
#         没有 CMakeLists，所以由本文件替它建一个静态库。
#
#   为什么用 GLOB_RECURSE：
#     上游模块随版本增删，逐个列 480 个 .c 只会得到一份永远在过期的清单。
#     CONFIGURE_DEPENDS 让 ninja 每次构建前检查目录，新增/删除源文件会
#     自动重新 Configure；不写它的话，加了文件必须手工重跑 cmake。
#
#   为什么 -w：
#     第三方代码不参与本工程的 -Werror 治理。上游的 warning 一旦变成
#     我们的 error，改不动（Third_Party 只读）也压不住（不能改源码）。
#
#   include 路径为什么是两处：
#     lvgl/          → 顶层转发头 lvgl.h、以及 lv_conf.h 的相对查找
#     lvgl/include   → 真正的公开聚合头 include/lvgl/lvgl.h 与私有头树
# =============================================================================

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/lvgl/src/*.c
)

add_library(lvgl STATIC ${LVGL_SOURCES})

target_include_directories(lvgl
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/lvgl
        ${CMAKE_CURRENT_SOURCE_DIR}/lvgl/include
)

# LVGL 自身要求 C99 以上；本工程全局 C11 已满足，这里只声明最低要求。
target_compile_features(lvgl PRIVATE c_std_99)

target_compile_options(lvgl PRIVATE -w)
