/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "svc_key_logic.h"
#include "bsp_iic.h"
#include "bsp_spi.h"
#include "bsp_uart.h"
#include "dev_spi.h"
#include "dev_iic.h"
#include "dev_uart.h"
void Task_UART(void *argument);
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
typedef StaticTask_t osStaticThreadDef_t;
typedef StaticQueue_t osStaticMessageQDef_t;
typedef StaticSemaphore_t osStaticMutexDef_t;
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
uint32_t defaultTaskBuffer[ 128 ];
osStaticThreadDef_t defaultTaskControlBlock;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .cb_mem = &defaultTaskControlBlock,
  .cb_size = sizeof(defaultTaskControlBlock),
  .stack_mem = &defaultTaskBuffer[0],
  .stack_size = sizeof(defaultTaskBuffer),
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for KEY_T */
osThreadId_t KEY_THandle;
const osThreadAttr_t KEY_T_attributes = {
  .name = "KEY_T",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityRealtime,
};
/* Definitions for KEY_CMD_T */
osThreadId_t KEY_CMD_THandle;
const osThreadAttr_t KEY_CMD_T_attributes = {
  .name = "KEY_CMD_T",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for LVGL_T */
osThreadId_t LVGL_THandle;
const osThreadAttr_t LVGL_T_attributes = {
  .name = "LVGL_T",
  .stack_size = 2048 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for UART_T */
osThreadId_t UART_THandle;
const osThreadAttr_t UART_T_attributes = {
  .name = "UART_T",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for KEY_EVT_Q */
osMessageQueueId_t KEY_EVT_QHandle;
uint8_t KEY_EVT_QBuffer[ 8 * sizeof( key_event_msg_t ) ];
osStaticMessageQDef_t KEY_EVT_QControlBlock;
const osMessageQueueAttr_t KEY_EVT_Q_attributes = {
  .name = "KEY_EVT_Q",
  .cb_mem = &KEY_EVT_QControlBlock,
  .cb_size = sizeof(KEY_EVT_QControlBlock),
  .mq_mem = &KEY_EVT_QBuffer,
  .mq_size = sizeof(KEY_EVT_QBuffer)
};
/* Definitions for IIC_LOCK */
osMutexId_t IIC_LOCKHandle;
osStaticMutexDef_t IIC_LOCKControlBlock;
const osMutexAttr_t IIC_LOCK_attributes = {
  .name = "IIC_LOCK",
  .cb_mem = &IIC_LOCKControlBlock,
  .cb_size = sizeof(IIC_LOCKControlBlock),
};
/* Definitions for SPI1_LOCK */
osMutexId_t SPI1_LOCKHandle;
osStaticMutexDef_t SPI1_LOCKControlBlock;
const osMutexAttr_t SPI1_LOCK_attributes = {
  .name = "SPI1_LOCK",
  .cb_mem = &SPI1_LOCKControlBlock,
  .cb_size = sizeof(SPI1_LOCKControlBlock),
};
/* Definitions for UART1_LOCK */
osMutexId_t UART1_LOCKHandle;
osStaticMutexDef_t UART1_LOCKControlBlock;
const osMutexAttr_t UART1_LOCK_attributes = {
  .name = "UART1_LOCK",
  .cb_mem = &UART1_LOCKControlBlock,
  .cb_size = sizeof(UART1_LOCKControlBlock),
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void Task_Key_Scan(void *argument);
void Task_Key_Cmd(void *argument);
void Task_LVGL(void *argument);
void Task_UART(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  Svc_Key_Logic_Init();   /* 以当前键电平作为"已确认"初态，避免上电误报 DOWN */
  /* USER CODE END Init */
  /* Create the mutex(es) */
  /* creation of IIC_LOCK */
  IIC_LOCKHandle = osMutexNew(&IIC_LOCK_attributes);

  /* creation of SPI1_LOCK */
  SPI1_LOCKHandle = osMutexNew(&SPI1_LOCK_attributes);

  /* creation of UART1_LOCK */
  UART1_LOCKHandle = osMutexNew(&UART1_LOCK_attributes);

  /* USER CODE BEGIN RTOS_MUTEX */
  /* 要互斥量的 BSP 初始化必须在这里：CubeMX 刚把这几把锁建出来，
   早一段（Init）句柄还是 NULL，会被 BSP_xxx_Init() 判成 ERR_STATE。
   设备层的虚拟外设跟在对应 BSP 总线之后：Dev_SPI / Dev_IIC 各自认领
   SPI1 / IIC 这把锁，并把片选、换速、超时这些"总线使用规则"接在
   BSP_SPI / BSP_IIC 之上。 */
  BSP_IIC_Init();
  Dev_IIC_Init();
  BSP_SPI_Init();
  Dev_SPI_Init();
  BSP_UART_Init();
  Dev_UART_Init();
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of KEY_EVT_Q */
  KEY_EVT_QHandle = osMessageQueueNew (8, sizeof(key_event_msg_t), &KEY_EVT_Q_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of KEY_T */
  KEY_THandle = osThreadNew(Task_Key_Scan, NULL, &KEY_T_attributes);

  /* creation of KEY_CMD_T */
  KEY_CMD_THandle = osThreadNew(Task_Key_Cmd, NULL, &KEY_CMD_T_attributes);

  /* creation of LVGL_T */
  LVGL_THandle = osThreadNew(Task_LVGL, NULL, &LVGL_T_attributes);

  /* creation of UART_T */
  UART_THandle = osThreadNew(Task_UART, NULL, &UART_T_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
__weak void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_Task_Key_Scan */
/**
* @brief Function implementing the KEY_T thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_Task_Key_Scan */
__weak void Task_Key_Scan(void *argument)
{
  /* USER CODE BEGIN Task_Key_Scan */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END Task_Key_Scan */
}

/* USER CODE BEGIN Header_Task_Key_Cmd */
/**
* @brief Function implementing the KEY_CMD_T thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_Task_Key_Cmd */
__weak void Task_Key_Cmd(void *argument)
{
  /* USER CODE BEGIN Task_Key_Cmd */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END Task_Key_Cmd */
}

/* USER CODE BEGIN Header_Task_LVGL */
/**
* @brief Function implementing the LVGL_T thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_Task_LVGL */
__weak void Task_LVGL(void *argument)
{
  /* USER CODE BEGIN Task_LVGL */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END Task_LVGL */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

