# =============================================================================
# BSP 层：硬件驱动层
#
#   每个模块一个静态库，只暴露自己的头文件目录给上层；
#   这一层直接调用 HAL，必须能看见 HAL 头，所以链接 stm32cubemx。
# =============================================================================

# -----------------------------------------------------------------------------
# bsp_gpio：GPIO 引脚级读写（经 HAL，一次一个引脚）
# -----------------------------------------------------------------------------
add_library(bsp_gpio STATIC
    BSP_GPIO/bsp_gpio.c
)
target_include_directories(bsp_gpio
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/BSP_GPIO
)
target_link_libraries(bsp_gpio
    PRIVATE stm32cubemx
)
target_compile_options(bsp_gpio PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# bsp_spi：SPI 总线（初始化 / 换速 / 搬字节）
#   只答三个问题，所以只需要 HAL，不再链接 bsp_gpio：片选在设备层（dev_spi），
#   本模块不认识任何具体设备。PUBLIC 只暴露 bsp_spi.h。
# -----------------------------------------------------------------------------
add_library(bsp_spi STATIC
    BSP_SPI/bsp_spi.c
)
target_include_directories(bsp_spi
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/BSP_SPI
)
target_link_libraries(bsp_spi
    PRIVATE stm32cubemx
)
target_compile_options(bsp_spi PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# bsp_iic：IIC 模块（总线句柄 + 互斥量排队 + DMA 完成唤醒）
#   I2C1 配成 400 kHz，收发都走 DMA1_Stream5/6；一次传输同步完成，
#   多任务按 IIC_LOCK 排队。
# -----------------------------------------------------------------------------
add_library(bsp_iic STATIC
    BSP_IIC/bsp_iic.c
)
target_include_directories(bsp_iic
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/BSP_IIC
)
target_link_libraries(bsp_iic
    PRIVATE stm32cubemx
)
target_compile_options(bsp_iic PRIVATE ${USER_WARN_FLAGS})

# -----------------------------------------------------------------------------
# bsp_uart：UART 模块（DMA 乒乓接收 + 环形发送队列 + 互斥量排队）
#   收发全程不阻塞调用者；接收侧单生产单消费无锁，发送侧靠 UART_LOCK 排队。
# -----------------------------------------------------------------------------
add_library(bsp_uart STATIC
    BSP_UART/bsp_uart.c
)
target_include_directories(bsp_uart
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/BSP_UART
)
target_link_libraries(bsp_uart
    PRIVATE stm32cubemx
)
target_compile_options(bsp_uart PRIVATE ${USER_WARN_FLAGS})
