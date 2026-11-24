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
 *       两种用法：
 *         - 单笔：Dev_IIC_Write() / Read() / WriteReg() / ReadReg()，
 *           各自完成"抢锁 → 换速 → 传输 → 放锁"；
 *         - 会话：Select() 一次 → 任意多笔读写 → Deselect() 一次。
 *           连续跟同一颗器件交换多组信息时用它，锁与换速只做一遍。
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
    DEV_IIC_ERR_HAL     = -6, /* HAL 报错（配置错或外设故障） */
} dev_iic_status_t;

/**
 * @brief IIC 总线句柄（不透明，定义在 dev_iic.c）
 * @note 只描述"用哪条 BSP 总线、哪把互斥量"，不含任何引脚或从机地址。
 */
typedef struct dev_iic dev_iic_t;

/**
 * @brief IIC 从机设备句柄（不透明，定义在 dev_iic.c）
 * @note 一个实例对应一颗器件：总线、7 bit 地址、总线时钟、子地址宽度。
 */
typedef struct dev_iic_dev dev_iic_dev_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/**
 * @brief I2C1 总线：BSP 总线句柄 + 互斥量句柄 + 运行块
 * @note Init 一次登记一条总线；同一条总线上的从机由 dev_iic_dev_* 携带。
 */
extern const dev_iic_t dev_iic1;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化一路 IIC 总线：检查总线、互斥量并探测 BSP 就绪
 * @param bus 总线句柄
 * @retval DEV_IIC_OK: 总线就绪；已初始化的总线直接返回
 * @retval DEV_IIC_ERR_STATE: 缺总线句柄、互斥量尚未创建，或 bsp_iic 未初始化
 *
 * @note 必须在 osMutexNew() 之后、bsp_iic 初始化之后调用。每条总线调一次，
 *       多路总线各调各的；本层不再持有总线表。
 */
dev_iic_status_t Dev_IIC_Init(const dev_iic_t *bus);

/**
 * @brief 取得总线所有权并切换到本设备的总线时钟（会话开始）
 * @param dev        设备句柄，不能为 NULL
 * @param timeout_ms 排队等锁的预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已持有总线，总线时钟已切到本设备
 * @retval DEV_IIC_ERR_PARAM: 句柄或总线为空
 * @retval DEV_IIC_ERR_STATE: 未初始化、调度器未运行，或本任务已在会话中
 * @retval DEV_IIC_ERR_TIMEOUT: 等锁超时
 * @retval DEV_IIC_ERR_NACK/ERR_BUS/ERR_HAL: 换速失败
 *
 * @note 与 Deselect() 成对，任务上下文调用。会话期间调用别的设备的读写
 *       会被拒绝（那次会话的速率是按前一台设的）；同一任务重复 Select
 *       同样被拒绝，不会重入。
 */
dev_iic_status_t Dev_IIC_Select(const dev_iic_dev_t *dev, uint32_t timeout_ms);

/**
 * @brief 归还总线（会话结束）
 * @param dev 设备句柄，不能为 NULL
 * @retval 无
 *
 * @note 只对"本任务、本设备"开的会话生效，其余情况直接返回——不会放掉
 *       别人的锁，也不会抬错设备。必须与 Select() 成对，任何出口都要调。
 */
void Dev_IIC_Deselect(const dev_iic_dev_t *dev);

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
 * @note 任务上下文调用。已在 Select() 开的会话里调用时只搬这一笔，
 *       不再抢锁与换速。
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
 * @note 任务上下文调用。已在 Select() 开的会话里调用时只搬这一笔，
 *       不再抢锁与换速。
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
 *       已在 Select() 开的会话里调用时只搬这一笔，不再抢锁与换速。
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
 *       已在 Select() 开的会话里调用时只搬这一笔，不再抢锁与换速。
 */
dev_iic_status_t Dev_IIC_ReadReg(const dev_iic_dev_t *dev, uint16_t reg,
                                 uint8_t *data, uint16_t len,
                                 uint32_t timeout_ms);

#endif /* __DEV_IIC_H__ */
