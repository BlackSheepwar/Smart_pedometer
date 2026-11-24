# =============================================================================
# OSAL：OS 访问归口（不是第 8 层，不进层间 DAG）
#
#   三个子目录按需建，本工程一个都没建——OS 全由 STM32CubeMX 提供：
#
#     文件夹1（Kernel/）本工程没移植 OS、缺 OS 源码本体时才建，放上游源码原样；
#               CubeMX 的 FreeRTOS 在 Middlewares/Third_Party，故不建。
#     文件夹2（Port/）  要自己写移植件时才建（节拍、上下文切换、临界区、堆、
#               FreeRTOSConfig.h）；CubeMX 已提供，故不建。
#     文件夹3（Objects/）本工程没配好任务 / 队列 / 互斥锁句柄、要在此创建对象时
#               才建；.ioc 已配好，对象由 Src/freertos.c 创建，故不建。
#
#   这一级只有两个头文件，按内外分工，配一个 INTERFACE 目标：
#     osal.h         文件1（RTOS 内）：包 OS 头 + extern 句柄，跑在 OS 上的
#                    模块只含它。
#     osal_types.h   文件2（RTOS 外）：建对象时要用的元素类型，只被注册点
#                    包含（本工程注册点是 Src/freertos.c，它不含 osal.h）。
#
#   被谁链接：只有直接碰 OS 的模块（Device 的 dev_iic/dev_spi/dev_uart、
#   Service 的 svc_key_logic、TASK 的 task_objects、以及 Src/freertos.c）。
#   BSP 一律不链：芯片驱动不认识 OS，换平台只重写 BSP 的 .c。
#
#   INTERFACE 目标不产 .o：本目录没有 .c，只负责把 include 路径与 OS 头
#   转给链接方。
# =============================================================================

add_library(osal INTERFACE)

target_include_directories(osal
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# osal.h 转出 OS 头，OS 的 include 路径必须跟着走：
# 链接 osal 的模块因此不需要再单独链 stm32cubemx 就能编译。
target_link_libraries(osal
    INTERFACE
        stm32cubemx
)
