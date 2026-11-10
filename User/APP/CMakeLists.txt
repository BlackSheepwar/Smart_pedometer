# =============================================================================
# APP 层：应用层
#
#   业务只有两种入口：Init（调度器启动前）和"一步"（线程里调用），
#   线程由 CubeMX 的 .ioc 配置、线程体在 User/TASK 里，本层不建线程、不碰内核。
#
#   一个模块一件事，目录名 = 模块名，文件名 = 模块名小写：
#     APP_Key_Cmd/app_key_cmd.c      按键命令表与处理函数（"哪个键做什么"）
#     APP_Screen/app_screen.c        界面生命周期、模式切换与数据同步
#     APP_UI_Port/app_ui_port.c      导出代码的本地适配层（唯一手写边界）
#
#   例外只有一个，且是有意的：
#     UI_Export/                     LVGL Pro 的导出物，纯生成，勿手改
#   它没有 APP_ 前缀，因为它不是一个模块——里面没有一行手写代码，
#   目录内容由编辑器整体覆盖，所以不参与本工程的命名与 -Werror 治理。
#
#   依赖方向单向：app_key_cmd → svc_key_cmd / app_screen，
#   app_screen → app_ui_port。
#   app_key_cmd → app_screen 这条边只用于"换面板"（写一个标志，见
#   app_screen.h 的 Toggle/Select/Step 拆分），它不引入任何 lv_ 类型；
#   反过来 app_screen 不依赖 app_key_cmd，所以两条边不会成环。
#
#   ★步数计数已删除★
#   天光仪表不显示步数，B10/B11 改作"直达面板"之后，APP_Step 既没有显示者
#   也没有写入者（只剩 112 B 代码 + 4 B RAM）。与其留一个没人调用的模块，
#   不如整体删掉：需要步数时按 git 历史 aed5f57 取回，或在数据源侧新建
#   一个带传感器的模块——那比复活一个空壳更直接。
#
#   头文件可见性：
#     app_key_cmd.h 不外泄任何下层类型，所以 svc_key_logic 用
#     PRIVATE：需要事件类型的是 app_key_cmd 自己（它 PUBLIC 链 svc_key_cmd）。
#     少一条 PUBLIC，上层就少一层"可以不经过我就能拿到下层"的机会。
# =============================================================================

# -----------------------------------------------------------------------------
# ui_export：LVGL Pro 导出物，纯生成
#
#   本库存在的唯一理由是"隔离生成代码"：它只被 app_ui_port PRIVATE 链接，
#   导出代码的符号（screen_hud、subject_*、字体指针）因此不会顺着
#   app_ui_port.h 传给 APP_Screen，更不会传到 TASK 层。
#
#   为什么不加 ${USER_WARN_FLAGS}：
#     里面没有一行是本工程写的。生成代码改不动（重新导出会整体覆盖），
#     把它纳入 -Werror 只会得到一份永远修不掉的报错。与 Third_Party 用 -w
#     同一个理由，区别是这里保留 -Wall -Wextra 的默认级别，只是不升级成
#     error，便于发现真正的问题。
#
#   为什么不用 CONFIGURE_DEPENDS 的 GLOB：
#     这些文件是导出产物的固定集合，重新导出时文件名不变；
#     显式列出来反而能在一份文件被删时立刻报错，而不是悄悄少编一个。
#     （中文字模那一项是唯一的例外，见下面的 if(EXISTS)：它要等界面
#      导出过一版带中文标签的 XML 之后才会出现。）
# -----------------------------------------------------------------------------
set(UI_EXPORT_SOURCES
    UI_Export/lvgl_ui_project.c
    UI_Export/lvgl_ui_project_gen.c
    UI_Export/screens/screen_hud_gen.c
    UI_Export/fonts/font_12_data.c
    UI_Export/fonts/font_14_data.c
    UI_Export/fonts/font_16_data.c
    UI_Export/fonts/font_num_28_data.c
    UI_Export/fonts/font_num_36_data.c
    UI_Export/fonts/font_num_44_data.c
)
# 中文字模（font_cjk_16）是"界面中文标签"那次导出的产物。用存在性判断
# 而不是直接列出来，是为了让两种导出状态都能直接编：
#   旧导出 → gen.c 不引用 font_cjk_16，文件不存在，跳过；
#   新导出 → 文件必然在，自动纳入（改完记得重跑一次 cmake 配置）。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/UI_Export/fonts/font_cjk_16_data.c)
    list(APPEND UI_EXPORT_SOURCES UI_Export/fonts/font_cjk_16_data.c)
endif()

add_library(ui_export STATIC ${UI_EXPORT_SOURCES})
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
# 字符串 subject 的缓冲长度。导出代码默认 UI_SUBJECT_STRING_LENGTH = 256，
# 而每个字符串 subject 要两份缓冲（当前值 + 上一值），本界面有 5 条
# （时间/日期/速度文本/纬度/经度），按默认值就是 5×2×256 = 2560 字节 RAM。
# 最长的一条是 "116.4074E"（9 字符），24 字节足够，顺带把越界截断收紧。
target_compile_definitions(ui_export PRIVATE UI_SUBJECT_STRING_LENGTH=24)

# -----------------------------------------------------------------------------
# app_ui_port：导出代码的本地适配层
#
#   本层是全工程唯一 include lvgl_ui_project.h 的地方，也是唯一出现
#   lv_ 类型的地方。它把"界面能显示什么"收敛成一串不带 UI 语义的函数
#   （App_UI_Init / Set_Mode / Set_Time / Set_Date / Set_Battery /
#   Set_Speed / Set_Position / Set_Satellites），于是 APP_Screen 一行 lv_ 都不用写。
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
#   本库只剩"表 + 处理函数"，没有任何对外函数：表是 const 数据（进 .rodata），
#   行数一起导出，由 TASK 把表和事件交给 svc_key_cmd。少一层纯转调的包装，
#   代价是表项类型与表长成为跨模块契约（见 app_key_cmd.h 的两条说明）。
#
#   svc_key_cmd 必须 PUBLIC：本库的公开头要暴露 svc_key_cmd_entry_t，
#   用它的代码（TASK）得顺带拿到 svc_key_cmd.h 与 svc_key_logic.h。
#   处理函数各自只碰一样东西：APP_Screen_Toggle_Mode() / Select_Mode()（标志）、
#   Dev_LED_Write()（GPIO），都不碰 LVGL。
#   本层因此既不 include FreeRTOS.h / cmsis_os2.h，也不依赖 svc_lvgl。
# -----------------------------------------------------------------------------
add_library(app_key_cmd STATIC
    APP_Key_Cmd/app_key_cmd.c
)
target_include_directories(app_key_cmd
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/APP_Key_Cmd
)
target_link_libraries(app_key_cmd
    PUBLIC  svc_key_cmd
    PRIVATE app_screen      # 只为 Toggle/Select_Mode()：写标志，不碰 LVGL
            dev_led
)
target_compile_options(app_key_cmd PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# app_screen：界面生命周期、模式切换与数据同步
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
)
target_compile_options(app_screen PRIVATE ${USER_WARN_FLAGS})