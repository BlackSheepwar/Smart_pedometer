# =============================================================================
# Dev 层：设备抽象层
#
#   把"一个按键""一颗灯""一块屏"封装成设备，对外只暴露能力，不暴露引脚。
#   每个模块在自己的 .c 里实例化引脚句柄与设备句柄，头文件只做 extern 声明；
#   本层没有集中式板级文件。
#   头文件里只有不透明的 dev_*_t（布局在 .c），所以 bsp_gpio / bsp_spi 一律
#   写 PRIVATE：需要它们的是本模块的 .c，上层既不需要也拿不到 BSP 的头。
#   虚拟外设（dev_spi：总线独占、片选、帧）也平铺在本层，实体设备（如
#   dev_st7789）只用它的头，不直接碰 BSP_SPI。
# =============================================================================

# -----------------------------------------------------------------------------
# dev_key：单个 GPIO 输入按键
# -----------------------------------------------------------------------------
add_library(dev_key STATIC
    Dev_Key/dev_key.c
)
target_include_directories(dev_key
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_Key
)
target_link_libraries(dev_key
    PRIVATE bsp_gpio
)
target_compile_options(dev_key PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_led：单个 LED
# -----------------------------------------------------------------------------
add_library(dev_led STATIC
    Dev_LED/dev_led.c
)
target_include_directories(dev_led
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_LED
)
target_link_libraries(dev_led
    PRIVATE bsp_gpio
)
target_compile_options(dev_led PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_spi：SPI 虚拟外设（总线独占 + 片选 + 帧 + 按设备换速）
#   对上只暴露"选设备 / 搬一帧"；芯片侧的 DMA 启动、完成回调、分频表、
#   错误恢复全在 BSP_SPI。片选引脚句柄来自 BSP_GPIO。
#   bsp_spi / bsp_gpio / stm32cubemx 都写 PRIVATE：需要它们的只有本模块的
#   .c，上层既不需要也拿不到 BSP 的类型。
# -----------------------------------------------------------------------------
add_library(dev_spi STATIC
    Dev_SPI/dev_spi.c
)
target_include_directories(dev_spi
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_SPI
)
target_link_libraries(dev_spi
    PRIVATE bsp_spi
    PRIVATE bsp_gpio
    PRIVATE stm32cubemx
)
target_compile_options(dev_spi PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_st7789：ST7789 SPI 彩屏（开窗 + 突发写像素）
#   头文件不暴露 dev_spi_t / bsp_gpio_t（句柄不透明），所以 dev_spi 与
#   bsp_gpio 都写 PRIVATE：需要它们的只有本模块的 .c。屏不再直接链 bsp_spi。
#   用它的层（Service 的 svc_lvgl）只链 dev_st7789 即可。
# -----------------------------------------------------------------------------
add_library(dev_st7789 STATIC
    Dev_ST7789/dev_st7789.c
)
target_include_directories(dev_st7789
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_ST7789
)
target_link_libraries(dev_st7789
    PRIVATE dev_spi
    PRIVATE bsp_gpio
)
target_compile_options(dev_st7789 PRIVATE ${USER_WARN_FLAGS})