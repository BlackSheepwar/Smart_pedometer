# =============================================================================
# User 顶层：按层收集子目录
#
#   BSP     第 1 层  硬件驱动层    GPIO 句柄与读写（引脚配置由 CubeMX 负责）
#   Device  第 2 层  设备抽象层    灯带、按键、单灯、OLED
#   Service 第 3 层  服务层        消抖、长按、连发
#   APP     第 4 层  应用层        档位业务：初始化 + 每步
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
# 移植来源：本目录整体来自 frame 工程（STM32F103，Cortex-M3），
#   搬到本工程（STM32F405，Cortex-M4F）。HAL 系列不同，但 User/ 这一层
#   只在 BSP 的 .c 里见过 HAL，所以除 bsp_gpio.c 换了一行头文件外，
#   五层的源码与链接关系与 frame 一致。
# =============================================================================

# 统一编译选项：警告即错误（子目录里直接引用这个变量）
set(USER_WARN_FLAGS -Wall -Wextra -Werror)

add_subdirectory(BSP)
add_subdirectory(Device)
add_subdirectory(Service)
add_subdirectory(APP)
add_subdirectory(TASK)

# -----------------------------------------------------------------------------
# 挂到主可执行文件：只列"主程序自己直接引用"的库
#   其它依赖（app_key_cmd、dev_led_bar 等）由各库自己的 PRIVATE 链带过来，
#   这里不再重复抄依赖树。新增模块要么由上一层 PUBLIC/PRIVATE 传递，
#   要么就是你真的在 freertos.c / main.c 里直接 include 了它。
# -----------------------------------------------------------------------------
target_link_libraries(${CMAKE_PROJECT_NAME}
    # TASK 层
    task_objects
    # BSP 层
    bsp_iic
    bsp_spi
    # Device 层
    dev_oled
    # Service 层
    svc_key_logic
    # APP 层：freertos.c 的启动链里直接 include app_level.h 调 APP_Level_Init()
    app_level
    # CubeMX
    stm32cubemx
)
