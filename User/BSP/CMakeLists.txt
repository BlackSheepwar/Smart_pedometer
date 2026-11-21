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
# bsp_iic：IIC 总线（初始化 / 换速 / 启动一次 DMA 搬运）
#   不持锁、不等待、不含 OS：总线独占、超时与错误码翻译在设备层（dev_iic）。
#   一次搬运只登记完成回调后返回，结果由回调给出。PUBLIC 只暴露 bsp_iic.h。
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
# bsp_uart：UART 总线（DMA 乒乓接收 + 环形发送队列）
#   不持锁、不等待、不含 OS：写入串行化、超时与发完判定在设备层（dev_uart）。
#   本层只做 DMA 收发、FIFO 搬运与错误重启。PUBLIC 只暴露 bsp_uart.h。
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
