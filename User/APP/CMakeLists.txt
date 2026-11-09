# =============================================================================
# APP 层：应用层
#
#   业务只有两种入口：Init（调度器启动前）和"一步"（线程里调用），
#   线程由 CubeMX 的 .ioc 配置、线程体在 User/TASK 里，本层不建线程、不碰内核。
#
#   一个模块一件事，目录名 = 模块名，文件名 = 模块名小写：
#     APP_Level/app_level.c          档位与显示（业务状态）
#     APP_Step/app_step.c            步数计数（业务状态，纯整数）
#     APP_Key_Cmd/app_key_cmd.c      按键命令表（事件 → 动作）
#     APP_Screen/app_screen.c        界面生命周期与数据同步
#     APP_UI_Port/app_ui_port.c      导出代码的本地适配层（唯一手写边界）
#
#   例外只有一个，且是有意的：
#     UI_Export/                     LVGL Pro 的导出物，纯生成，勿手改
#   它没有 APP_ 前缀，因为它不是一个模块——里面没有一行手写代码，
#   目录内容由编辑器整体覆盖，所以不参与本工程的命名与 -Werror 治理。
#
#   依赖方向单向：app_key_cmd → app_step / app_level，app_screen → app_ui / app_step。
#   两个"使用者"都只依赖业务状态 app_step，彼此之间没有边——
#   按键不需要知道界面存在，界面也不需要知道按键存在。
#
#   头文件可见性：
#     app_level.h 不外泄任何下层类型，所以 svc_key_logic 与 dev_led_bar 都用
#     PRIVATE：需要事件类型的是 app_key_cmd（它自己 PUBLIC 链 svc_key_logic），
#     不是 app。TASK 层链 app_key_cmd 就能看见 svc_key_logic.h。
#     少一条 PUBLIC，上层就少一层"可以不经过我就能拿到下层"的机会。
# =============================================================================

# -----------------------------------------------------------------------------
# app_level：档位与显示
# -----------------------------------------------------------------------------
add_library(app_level STATIC
    APP_Level/app_level.c
)
target_include_directories(app_level
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/APP_Level
)
target_link_libraries(app_level
    PRIVATE dev_led_bar
)
target_compile_options(app_level PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# app_step：步数计数（业务状态）
#   无任何依赖：不 include lvgl.h，也不 include 任何 OS 头文件，
#   只有一个 int32_t 和三个极短的函数。单写者/单读者的线程模型见头文件。
# -----------------------------------------------------------------------------
add_library(app_step STATIC
    APP_Step/app_step.c
)
target_include_directories(app_step
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/APP_Step
)
target_compile_options(app_step PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# ui_export：LVGL Pro 导出物，纯生成
#
#   本库存在的唯一理由是"隔离生成代码"：它只被 app_ui_port PRIVATE 链接，
#   导出代码的符号（screen1、subject_steps、字体指针）因此不会顺着
#   app_ui_port.h 传给 APP_Screen，更不会传到 TASK 层。
#
#   为什么不加 ${USER_WARN_FLAGS}：
#     里面没有一行是本工程写的。生成代码改不动（重新导出会整体覆盖），
#     把它纳入 -Werror 只会得到一份永远修不掉的报错。与 Third_Party 用 -w
#     同一个理由，区别是这里保留 -Wall -Wextra 的默认级别，只是不升级成
#     error，便于发现真正的问题。
#
#   为什么不用 CONFIGURE_DEPENDS 的 GLOB：
#     这 7 个文件是导出产物的固定集合，重新导出时文件名不变；
#     显式列出来反而能在一份文件被删时立刻报错，而不是悄悄少编一个。
# -----------------------------------------------------------------------------
add_library(ui_export STATIC
    UI_Export/lvgl_ui_project.c
    UI_Export/lvgl_ui_project_gen.c
    UI_Export/screens/screen1_gen.c
    UI_Export/fonts/font_12_data.c
    UI_Export/fonts/font_14_data.c
    UI_Export/fonts/font_16_data.c
    UI_Export/fonts/font_num_36_data.c
)
# PUBLIC 是给 app_ui_port 用的：它要 include lvgl_ui_project.h。
# app_ui_port 链本库用的是 PRIVATE，所以这层符号不会继续往上冒。
target_include_directories(ui_export
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/UI_Export
)
# PUBLIC 而不是 PRIVATE：本库的公开头（lvgl_ui_project_gen.h）自己就
# include 了 lvgl/lvgl.h，用它的代码必须同时拿到 lvgl 的 include 目录。
# 这不违反"lvgl 不外泄"的约定——app_ui_port 链本库用的是 PRIVATE，
# 所以这层依赖到 app_ui_port 为止，不会再传到 APP_Screen。
target_link_libraries(ui_export
    PUBLIC lvgl
)
# 生成代码在 LV_USE_XML 关闭时会留下一个只服务于 XML 路径的静态函数
# （lvgl_ui_project_gen.c 末尾的 check_font），必然报 -Wunused-function。
# 这条告警没有动作价值——改不动它，重新导出还会回来——所以定向关掉。
target_compile_options(ui_export PRIVATE -Wno-unused-function)

# -----------------------------------------------------------------------------
# app_ui_port：导出代码的本地适配层
#
#   本层是全工程唯一 include lvgl_ui_project.h 的地方，也是唯一出现
#   lv_ 类型的地方。它把"界面能做什么"收敛成两个不带 UI 语义的函数
#   （App_UI_Init / App_UI_Set_Steps），于是 APP_Screen 一行 lv_ 都不用写。
#
#   手写代码，因此参与 -Werror 治理。把它和 ui_export 分成两个库，
#   正是为了这一点：同一个 target 内没法给部分源文件加不同的编译选项。
# -----------------------------------------------------------------------------
add_library(app_ui_port STATIC
    APP_UI_Port/app_ui_port.c
)
target_include_directories(app_ui_port
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/APP_UI_Port
)
target_link_libraries(app_ui_port
    PRIVATE ui_export
)
target_compile_options(app_ui_port PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# app_key_cmd：按键命令表
#   B10 的处理函数调 APP_Step_Add()——只改一个整数，不碰 LVGL、
#   不碰信号量。本层因此既不 include FreeRTOS.h / cmsis_os2.h，
#   也不依赖 svc_lvgl（上一版调 Svc_LVGL_Request_Update 的那条边已删除）。
# -----------------------------------------------------------------------------
add_library(app_key_cmd STATIC
    APP_Key_Cmd/app_key_cmd.c
)
target_include_directories(app_key_cmd
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/APP_Key_Cmd
)
target_link_libraries(app_key_cmd
    PRIVATE svc_key_logic
            app_level
            app_step
            dev_led
)
target_compile_options(app_key_cmd PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# app_screen：界面生命周期与数据同步
#   头文件不出现 lv_obj_t，也不出现任何 lv_ 类型：界面符号被
#   APP_UI_Port/app_ui_port.h 挡在下面，所以 lvgl 连 PRIVATE 都不必写在这里
#   ——app_screen.c 一行 lv_ 都没有。这是"界面实现可整体替换"
#   的结构保证：换一个 UI 只改 UI_Export 与 APP_UI_Port，本文件不动。
# -----------------------------------------------------------------------------
add_library(app_screen STATIC
    APP_Screen/app_screen.c
)
target_include_directories(app_screen
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/APP_Screen
)
target_link_libraries(app_screen
    PRIVATE app_ui_port
    PRIVATE app_step
)
target_compile_options(app_screen PRIVATE ${USER_WARN_FLAGS})