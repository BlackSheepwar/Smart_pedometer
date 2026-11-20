# =============================================================================
# User 顶层：按层收集子目录
#
#   Third_Party 第 0 层  第三方只读区  LVGL 源码
#   BSP     第 1 层  硬件驱动层    GPIO 句柄与读写、SPI 总线/设备、IIC 总线、
#                                UART（引脚与时钟由 CubeMX 负责）
#   Device  第 2 层  设备抽象层    按键、单灯、ST7789 屏（不含 GUI 概念）
#   Service 第 3 层  服务层        消抖/长按/连发、命令表分发、LVGL 显示绑定
#   APP     第 4 层  应用层        界面生命周期、模式面板与数据同步
#   TASK    任务层   线程体        一个任务一个 .c，覆盖 CubeMX 的 __weak 入口
#
# 分层规则由"链接关系"保证，不靠自觉：
#   每层的模块都是一个静态库，只链接下一层的模块。
#   Device 头文件里出现 bsp_gpio_t 指针，所以 bsp_gpio 是 PUBLIC 传递的，
#   上层能看见 BSP 的声明，但句柄里的端口号和引脚号由 BSP 独占。
#
# 线程由 CubeMX 的 .ioc 生成（osThreadNew 在 Src/freertos.c），
# User/ 这边只写线程体和业务，不手写 osThreadNew。
#
# 换 MCU 只改 BSP 的 .c 与 HAL 头：Device 及以上不认识 HAL 类型，
# 这是"上层源码与芯片型号解耦"的可移植性判据，不在注释里留移植记录。
# =============================================================================

# 统一编译选项：警告即错误（子目录里直接引用这个变量）
set(USER_WARN_FLAGS -Wall -Wextra -Werror)

# Third_Party 必须排在 BSP 之前：Service 层的 svc_lvgl 要链 lvgl
# 目标必须先存在；CMake 的目标解析虽然允许后置定义，但把依赖方写在
# 被依赖方之前会让"谁依赖谁"在文件里读不出来。
add_subdirectory(Third_Party)
add_subdirectory(BSP)
add_subdirectory(Device)
add_subdirectory(Service)
add_subdirectory(APP)
add_subdirectory(TASK)

# -----------------------------------------------------------------------------
# 挂到主可执行文件：只列"主程序自己直接引用"的库
#   其它依赖（app_key_cmd、dev_led 等）由各库自己的 PRIVATE 链带过来，
#   这里不再重复抄依赖树。新增模块要么由上一层 PUBLIC/PRIVATE 传递，
#   要么就是你真的在 freertos.c / main.c 里直接 include 了它。
# -----------------------------------------------------------------------------
target_link_libraries(${CMAKE_PROJECT_NAME}
    # TASK 层
    task_objects
    # BSP 层
    bsp_iic
    bsp_spi
    bsp_uart
    # Device 层（虚拟外设：freertos.c 里直接调 Dev_SPI_Init）
    dev_spi
    # Service 层
    svc_key_logic
    # APP 层（app_key_cmd / app_screen / app_ui_port 由各自模块的内部依赖引入）
    # CubeMX
    stm32cubemx
)
