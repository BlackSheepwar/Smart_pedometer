# =============================================================================
# Dev 层：设备抽象层
#
#   把物理设备和虚拟总线封装为设备能力，不暴露芯片引脚。
#   BSP 句柄在模块 .c 内部使用，模块头文件不泄漏 HAL 类型。
# =============================================================================

# -----------------------------------------------------------------------------
# dev_key：单个 GPIO 输入按键
# -----------------------------------------------------------------------------
add_library(dev_key STATIC
    Dev_Key/dev_key.c
)
target_include_directories(dev_key PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_Key)
target_link_libraries(dev_key PRIVATE bsp_gpio)
target_compile_options(dev_key PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_led：单个 LED
# -----------------------------------------------------------------------------
add_library(dev_led STATIC
    Dev_LED/dev_led.c
)
target_include_directories(dev_led PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_LED)
target_link_libraries(dev_led PRIVATE bsp_gpio)
target_compile_options(dev_led PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_spi：SPI 虚拟外设
# -----------------------------------------------------------------------------
add_library(dev_spi STATIC
    Dev_SPI/dev_spi.c
)
target_include_directories(dev_spi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_SPI)
target_link_libraries(dev_spi PRIVATE bsp_spi bsp_gpio stm32cubemx)
target_compile_options(dev_spi PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_iic：IIC 虚拟外设
# -----------------------------------------------------------------------------
add_library(dev_iic STATIC
    Dev_IIC/dev_iic.c
)
target_include_directories(dev_iic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_IIC)
target_link_libraries(dev_iic PRIVATE bsp_iic stm32cubemx)
target_compile_options(dev_iic PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_uart：UART 虚拟外设与可替换 OS 端口
# -----------------------------------------------------------------------------
add_library(dev_uart STATIC
    Dev_UART/dev_uart.c
    Dev_UART/dev_uart_port_freertos.c
)
target_include_directories(dev_uart PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_UART)
target_link_libraries(dev_uart PRIVATE bsp_uart stm32cubemx)
target_compile_options(dev_uart PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# dev_st7789：ST7789 SPI 显示设备
# -----------------------------------------------------------------------------
add_library(dev_st7789 STATIC
    Dev_ST7789/dev_st7789.c
)
target_include_directories(dev_st7789 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Dev_ST7789)
target_link_libraries(dev_st7789 PRIVATE dev_spi bsp_gpio)
target_compile_options(dev_st7789 PRIVATE ${USER_WARN_FLAGS})
