/**
 * @file dev_iic.h
 * @brief IIC 虚拟外设：总线独占、地址与速率绑定、等待与超时
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 本层只导出设备句柄：总线对象定义在 .c 里，出头文件的只有设备类型，
 *       总线类型连 typedef 都不出现，也就没有人能 include 到它。
 *       一颗从机器件对应一个设备实例，实例里写死它的 7 bit 地址、总线
 *       时钟与寄存器子地址宽度；上层的每一笔读写都以设备句柄为入口，
 *       不需要知道从机地址，也不需要知道总线是谁。
 *
 *       本层负责"谁现在用这条总线、等多久、超时怎么办、错误码怎么翻译"；
 *       芯片侧的 DMA 启动、完成中断、外设复位都在 bsp_iic。换芯片只改
 *       bsp_iic 的实现，本层的接口不变。
 *
 *       设备实例在接入器件时登记：一颗从机器件一个实例，写在 .c 的移植
 *       段里。没有器件时不产生动作，接口仍然可用，接入后行为不变。
 */

#ifndef __DEV_IIC_H__
#define __DEV_IIC_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief DEV_IIC 返回码
 * @note 与 bsp_iic_status_t 一一对应；上层只认这一套，BSP 的枚举不出本层。
 */
typedef enum {
    DEV_IIC_OK          = 0,  /* 成功 */
    DEV_IIC_ERR_PARAM   = -1, /* 参数非法 */
    DEV_IIC_ERR_STATE   = -2, /* 未初始化，或同一任务重复进入 */
    DEV_IIC_ERR_TIMEOUT = -3, /* 等总线超时，或搬运超时（已中止并复位） */
    DEV_IIC_ERR_NACK    = -4, /* 从机 NACK */
    DEV_IIC_ERR_BUS     = -5, /* 总线错误 */
    /* -6 空号：数值即契约，重排会把调用者手里的旧比较结果静默改掉 */
    DEV_IIC_ERR_HAL     = -7, /* HAL 报错（配置错或外设故障） */
} dev_iic_status_t;

/**
 * @brief IIC 从机设备句柄（不透明，定义在 dev_iic.c）
 * @note 一个实例对应一颗器件：总线、7 bit 地址、总线时钟、子地址宽度。
 */
typedef struct dev_iic_dev dev_iic_dev_t;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化 IIC 虚拟外设：逐路检查总线、互斥量并探测 BSP 就绪
 * @retval DEV_IIC_OK: 全部总线就绪
 * @retval DEV_IIC_ERR_STATE: 缺总线句柄、互斥量尚未创建，或 bsp_iic 未初始化
 * @note 必须在 osMutexNew() 之后、bsp_iic 初始化之后调用。
 */
dev_iic_status_t Dev_IIC_Init(void);

/**
 * @brief 向从机直接写数据（RAW 模式，无寄存器子地址）
 * @param dev        设备句柄，不能为 NULL
 * @param data       发送缓冲区，不能为 NULL
 * @param len        发送字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已发送完成
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化，或同一任务重复进入
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 * @note 任务上下文调用；同一任务的嵌套调用会被拒绝。
 */
dev_iic_status_t Dev_IIC_Write(const dev_iic_dev_t *dev,
                               const uint8_t *data, uint16_t len,
                               uint32_t timeout_ms);

/**
 * @brief 从从机直接读数据（RAW 模式，无寄存器子地址）
 * @param dev        设备句柄，不能为 NULL
 * @param data       接收缓冲区，不能为 NULL
 * @param len        接收字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已读满 len 字节
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化，或同一任务重复进入
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 * @note 任务上下文调用；同一任务的嵌套调用会被拒绝。
 */
dev_iic_status_t Dev_IIC_Read(const dev_iic_dev_t *dev,
                              uint8_t *data, uint16_t len,
                              uint32_t timeout_ms);

/**
 * @brief 向寄存器子地址写数据（MEM 模式）
 * @param dev        设备句柄，不能为 NULL；子地址宽度取自设备句柄
 * @param reg        寄存器子地址
 * @param data       发送缓冲区，不能为 NULL
 * @param len        发送字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已写入完成
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化，或同一任务重复进入
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 * @note 子地址的写 + repeated START + 数据由一次 DMA 完成，中途不放弃总线。
 */
dev_iic_status_t Dev_IIC_WriteReg(const dev_iic_dev_t *dev, uint16_t reg,
                                  const uint8_t *data, uint16_t len,
                                  uint32_t timeout_ms);

/**
 * @brief 从寄存器子地址读数据（MEM 模式）
 * @param dev        设备句柄，不能为 NULL；子地址宽度取自设备句柄
 * @param reg        寄存器子地址
 * @param data       接收缓冲区，不能为 NULL
 * @param len        接收字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已读满 len 字节
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化，或同一任务重复进入
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 * @note 子地址的写 + repeated START + 接收由一次 DMA 完成。
 */
dev_iic_status_t Dev_IIC_ReadReg(const dev_iic_dev_t *dev, uint16_t reg,
                                 uint8_t *data, uint16_t len,
                                 uint32_t timeout_ms);

#endif /* __DEV_IIC_H__ */
