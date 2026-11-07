# =============================================================================
# Svc 层：服务层
#
#   存放"设备怎么用"的业务规则：消抖、长按、连发。
#   不碰寄存器，只调用 Dev 层暴露的设备句柄与函数。
#
#   按键事件投递出口只有一个：svc_key_logic.c 的 Svc_Key_Logic_SendEvent()。
#   它投的是 CubeMX 生成的消息队列 KEY_EVT_Q（Smart_pedometer.ioc 的 Queues01）。
#   想换后端（环形缓冲、直接回调）只改那一个函数体，不要另建并行组件：
#   并行的"备选实现"没人调用，既占维护成本又容易和真实路径走偏。
#
#   头文件里出现了 dev_key_t 指针（事件按设备句柄回传），所以 dev_key 必须
#   PUBLIC 传递：上层只拿到 svc_key_logic 的头文件路径，看不见 Dev_Key 目录，
#   就会报 dev_key.h: No such file or directory。
# =============================================================================

# -----------------------------------------------------------------------------
# 消抖服务
# -----------------------------------------------------------------------------
add_library(svc_key_logic STATIC
    Svc_Key_Logic/svc_key_logic.c
)
target_include_directories(svc_key_logic
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Svc_Key_Logic
)
target_link_libraries(svc_key_logic
    PRIVATE stm32cubemx
    PUBLIC  dev_key
)
target_compile_options(svc_key_logic PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# svc_lvgl：把 LVGL 的显示服务绑定到 ST7789 设备上
#   第三方库的适配器放在 Service（跨设备组合），不放 Device：
#   Device 层只保留"窗怎么开、像素怎么写"，不含任何 GUI 概念。
#   本模块是 LVGL 在本工程里的唯一适配点：lv_init / tick / display / flush。
#   毫秒时基由 TASK 层以函数指针注入，故本层不读时钟、不引 HAL / OS 头。
#
#   头文件不暴露 lv_ 类型，也不暴露 dev_st7789_t，所以 dev_st7789 与 lvgl
#   都写 PRIVATE：别人的类型不会顺着头文件传给 APP / TASK。
# -----------------------------------------------------------------------------
add_library(svc_lvgl STATIC
    Svc_LVGL/svc_lvgl.c
)
target_include_directories(svc_lvgl
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Svc_LVGL
)
target_link_libraries(svc_lvgl
    PRIVATE dev_st7789
    PRIVATE lvgl
    # 只为 cmsis_os2.h：Svc_LVGL_Request_Switch() 要释放二值信号量。
    # 头文件里不出现任何 OS 类型，所以是 PRIVATE。
    PRIVATE stm32cubemx
)
target_compile_options(svc_lvgl PRIVATE ${USER_WARN_FLAGS})
