# =============================================================================
# Third_Party 层：别人写的代码，只读
#
#   lvgl：LVGL 9.6.0 核心库（上游 v9.6.0 正式版 tag，非开发分支）。
#         只保留 src 与 include 两棵树 + 三个转发头 + lv_conf_template.h，
#         上游自带的 CMakeLists 未拷入，所以由本文件替它建一个静态库。
#
#   只收 *.c 是有意的：
#     上游 src/ 下还有 67 个 .cpp（thorvg、vg_lite_tvg、lovyan_gfx、
#     tft_espi、arduino fs）。本工程是纯 C，这些文件既不该编也编不过；
#     对应功能由 lv_conf.h 里的 LV_USE_* 全部关掉，不链接即可。
#
#   lv_conf.h 的位置：
#     放在本目录的 lvgl/ 下，靠 -I lvgl 被 lv_conf_internal.h 的
#     __has_include("lv_conf.h") 探测到，进而自动开 LV_CONF_INCLUDE_SIMPLE。
#     若哪天探测失效（换到不支持 __has_include 的编译器），把它移到
#     User/Third_Party/lv_conf.h（与 lvgl 同级）即可走默认回退路径。
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

# -----------------------------------------------------------------------------
# ringbuffer：镜像索引环形缓冲（上游 W-Okaka/ringbuffer，public domain）
#
#   保持上游源码与目录名不变：第三方代码只读，改由本文件替它建静态库。
#   头文件里的 ringbuffer_status() 声明与 .c 里的 static inline 定义不一致，
#   外部翻译单元调用会链接失败；本工程不使用该函数，靠 -w 让上游自身
#   编译通过。NDEBUG 关掉库内的 assert，避免把 assert/printf 拉进固件。
# -----------------------------------------------------------------------------
add_library(ringbuffer STATIC
    ringbuffer/ringbuffer.c
)

target_include_directories(ringbuffer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/ringbuffer
)

target_compile_features(ringbuffer PRIVATE c_std_99)
target_compile_definitions(ringbuffer PRIVATE NDEBUG)
target_compile_options(ringbuffer PRIVATE -w)
