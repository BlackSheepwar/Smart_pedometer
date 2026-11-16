/**
 * @file bsp_gpio.h
 * @brief 硬件驱动层：GPIO 句柄与读写函数
 * @author Ricardo
 * @date 2026-10-23
 *
 * @note 一个引脚一个 bsp_gpio_t 句柄，上层拿到句柄指针调函数即可读写。
 *       引脚的方向、上下拉、初值全部由 CubeMX 生成的 MX_GPIO_Init() 配置，
 *       本层只负责"把句柄翻译成 GPIOx + Pin"，不重复配置硬件。
 *
 *       句柄里带一个 dir 字段，是给"方向不能靠配置、只能靠代码说明"的
 *       芯片留的位置（需要运行时切输入/输出的 MCU）；方向由 CubeMX 固定的
 *       平台上，这个字段只作为自描述与写入保护用。
 *
 *       端口字段故意用 void * 而不是 GPIO_TypeDef *：
 *       HAL 的 GPIO_TypeDef 是匿名 struct 的 typedef，没法前置声明，
 *       一旦写进头文件，stm32f4xx_hal.h 就会顺着 Device / Service / APP
 *       的头文件链传染上去，而这些层本来不该认识 HAL。
 *       真正的类型转换只发生在 bsp_gpio.c 内部。
 */

#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 引脚方向
 * @note 方向由 CubeMX 在 MX_GPIO_Init() 里定死时只会用到 IN 或 OUT；
 *       IN_OUT 是给可运行时换向的 MCU 留的扩展位，当前平台不使用。
 */
typedef enum {
    BSP_GPIO_DIR_IN     = 0,            /* 输入：只读，写它会被忽略 */
    BSP_GPIO_DIR_OUT    = 1,            /* 输出：可写，也可读回引脚实际电平 */
    BSP_GPIO_DIR_IN_OUT = 2,            /* 双向：输入输出可运行时切换 */
} bsp_gpio_dir_t;

/**
 * @brief GPIO 引脚句柄（不透明类型，成员定义在 bsp_gpio.c）
 */
typedef struct bsp_gpio bsp_gpio_t;

/*==============================================================================
 * 外部声明：每个用到的引脚一份句柄
 *============================================================================*/
/* 与 bsp_gpio.c 的实例表一一对应；行尾是引脚在本板的用途。 */
extern const bsp_gpio_t bsp_gpio_pb2;   /* LED */
extern const bsp_gpio_t bsp_gpio_pc13;  /* 按键 */
extern const bsp_gpio_t bsp_gpio_pa8;   /* 显示片选 */
extern const bsp_gpio_t bsp_gpio_pa4;   /* 触摸片选 */
extern const bsp_gpio_t bsp_gpio_pc4;   /* 触摸中断 */
extern const bsp_gpio_t bsp_gpio_pb11;  /* 显示 DC */
extern const bsp_gpio_t bsp_gpio_pb12;  /* 显示 RST */

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 读取引脚电平
 * @param gpio 引脚句柄
 * @retval 1: 高电平
 * @retval 0: 低电平
 * @note 句柄为空返回 0。
 */
uint8_t BSP_GPIO_Read(const bsp_gpio_t *gpio);

/**
 * @brief 设置引脚输出电平
 * @param gpio  引脚句柄
 * @param level 1 为高，0 为低
 * @note 只影响这一个引脚，同端口的其他引脚不受影响。
 *       方向是 BSP_GPIO_DIR_IN 的句柄会被直接忽略（写输入没有意义）。
 */
void BSP_GPIO_Write(const bsp_gpio_t *gpio, uint8_t level);

/**
 * @brief 翻转引脚输出电平
 * @param gpio 引脚句柄
 * @note 只用于单写者场景（读 ODR 再写）；输入句柄同样会被忽略。
 */
void BSP_GPIO_Toggle(const bsp_gpio_t *gpio);

#endif /* __BSP_GPIO_H__ */
