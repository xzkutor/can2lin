/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "main.h"
#include "can.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define CAN_ID_BRIGHTNESS 0x635U
#define CAN_ID_TIME       0x65DU

#define LIN_SEND_INTERVAL_MS  500
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile uint8_t linBrightness = 0;
volatile uint8_t linHour = 0;
volatile uint8_t linMinute = 0;
volatile uint8_t linSecond = 0;
volatile uint8_t hasData = 0;
volatile uint8_t sleep_flag = 0;
volatile uint32_t lastCanRx = 0;
static volatile uint32_t lastLinSendTick = 0;

CAN_RxHeaderTypeDef   RxHeader;
uint8_t               RxData[8];

typedef struct {
  CAN_RxHeaderTypeDef header;
  uint8_t  data[8];
} CanFrame;

#define CAN_RX_BUFFER_SIZE 32
static CanFrame rxBuffer[CAN_RX_BUFFER_SIZE];
static volatile uint8_t rxHead = 0, rxTail = 0;

/* last value data */
static volatile uint8_t lastBrightness = 0xFF;
static volatile uint8_t lastHour       = 0xFF;
static volatile uint8_t lastMinute     = 0xFF;
static volatile uint8_t lastSecond     = 0xFF;

/* change flags */
static volatile bool flagBrightness = false;
static volatile bool flagTime       = false;


/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void Enable_CAN(void);
void Disable_CAN(void);
void Enable_LIN(void);
void Disable_LIN(void);
static void sendLIN_Data(uint8_t, uint8_t, uint8_t, uint8_t);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
inline void Enable_CAN(void)
{
	HAL_GPIO_WritePin(CAN_LED_GPIO_Port, CAN_LED_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(CAN_STB_GPIO_Port, CAN_STB_Pin, GPIO_PIN_RESET);
}

inline void Disable_CAN(void)
{
	HAL_GPIO_WritePin(CAN_LED_GPIO_Port, CAN_LED_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(CAN_STB_GPIO_Port, CAN_STB_Pin, GPIO_PIN_SET);
}

inline void Enable_LIN(void)
{
	HAL_GPIO_WritePin(LIN_LED_GPIO_Port, LIN_LED_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(LIN_SLP_GPIO_Port, LIN_SLP_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(LIN_WAKE_GPIO_Port, LIN_WAKE_Pin, GPIO_PIN_SET);
}

inline void Disable_LIN(void)
{
	HAL_GPIO_WritePin(LIN_WAKE_GPIO_Port, LIN_WAKE_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(LIN_SLP_GPIO_Port, LIN_SLP_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(LIN_LED_GPIO_Port, LIN_LED_Pin, GPIO_PIN_RESET);
}

void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_11);
}

static inline uint8_t decodeHour(uint8_t b5, uint8_t b6) {
    return (b5 >> 4) | ((b6 & 0x01) << 4);
}
static inline uint8_t decodeMinute(uint8_t b6) {
    return (b6 & 0x7F) >> 1;
}
static inline uint8_t decodeSecond(uint8_t b7, uint8_t b6) {
    return (b7 << 1) | ((b6 & 0x80) >> 7);
}

/* Main loop - process buffer and accumulate changes */
void processCanFrames(void)
{
    while (rxTail != rxHead) {
        CanFrame *f = &rxBuffer[rxTail];
        rxTail = (rxTail + 1) % CAN_RX_BUFFER_SIZE;

        switch (f->header.StdId) {
            case CAN_ID_BRIGHTNESS: {
                uint8_t b = f->data[0];
                if (b != lastBrightness) {
                    lastBrightness = b;
                    flagBrightness = true;
                }
                break;
            }
            case CAN_ID_TIME: {
                uint8_t h = decodeHour(f->data[5], f->data[6]);
                uint8_t m = decodeMinute(f->data[6]);
                uint8_t s = decodeSecond(f->data[7], f->data[6]);
                if (h != lastHour || m != lastMinute || s != lastSecond) {
                    lastHour   = h;
                    lastMinute = m;
                    lastSecond = s;
                    flagTime = true;
                }
                break;
            }
            default:
                // Ignore all other IDs
                break;
        }
    }

    uint32_t now = HAL_GetTick();

    /* if there any change then make and send the LIN packet*/
    if ((flagBrightness || flagTime) && (now - lastLinSendTick >= LIN_SEND_INTERVAL_MS)) {

    	Enable_LIN();
        sendLIN_Data(lastBrightness, lastHour, lastMinute, lastSecond);
        lastLinSendTick = now;
        Disable_LIN();

        // turnoff the flags
        flagBrightness = false;
        flagTime       = false;
    }
}

static void enter_stop_and_resume(void)
{
	Disable_CAN();

	__HAL_RCC_AFIO_CLK_ENABLE();
	AFIO->EXTICR[2] = (AFIO->EXTICR[2] & ~(0x0F << 12)) |  (AFIO_EXTICR3_EXTI11_PA << 12);

	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Pin  = GPIO_PIN_11;
	GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

	__HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_11);

	// turn on EXTI for wake up
	HAL_NVIC_SetPriority(EXTI15_10_IRQn, 2, 0);
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);


    /* Stop TIM2 */
    HAL_TIM_Base_Stop_IT(&htim2);

    /* prepare EXTI as wake source */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);

    /* Enter Stop Mode (wake up via EXTI11) */
    //HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
    HAL_PWR_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFI);
    /* === WakeUP code === */

    /* 1) reconfigure clocking */
    SystemClock_Config();

    /* 2) reconfigure peripherals */
    MX_GPIO_Init();
    MX_CAN_Init();
    HAL_CAN_Start(&hcan);
    HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING);

    MX_TIM2_Init();
    HAL_TIM_Base_Start_IT(&htim2);

    /* 3) reset inactivity timer */
    lastCanRx = HAL_GetTick();
    Enable_CAN();
}

static uint8_t LIN_Checksum(uint8_t *data, uint8_t length)
{
    uint16_t crc = 0;
    for (uint8_t i = 1; i < length; i++) {
        crc += data[i];
        if (crc > 0xFF)
            //crc = (crc & 0xFF) + (crc >> 8);
            crc = ((crc >> 8) & 0xFF) + (crc & 0xFF);
    }
    //return crc & 0xFF;
    return (0xFF - (crc & 0xFF));
}

static void sendLIN_Data(uint8_t brightness, uint8_t hour, uint8_t min, uint8_t sec)
{
    uint8_t frame[16];

    /* Build LIN frame: Sync, PID, Data, Checksum */
    frame[0] = 0x55;        /* Sync byte */
    frame[1] = 0x73;        /* PID for brightness (same as PIC implementation) 0x33 */

    frame[2] = brightness+20;  /* Brightness data */

    // Sec
    frame[5] = (sec & 0x7F) << 1;

    // Min
    frame[5] |= ((min & 0x3F) >> 5);
    frame[4] = ((min & 0x3F) << 3);

    // Hour
    frame[4] |= ((hour & 0x1F) >> 2);
    frame[3] = (hour & 0x1F) << 6;



    if(brightness > 0) {
    	frame[3] |= 0x0B;
    } else {
        frame[3] |= 0x01;
    }

    frame[6] = 0xFF;
    frame[7] = 0xFF;
    frame[8] = 0xFF;
    frame[9] = 0xFF;

    frame[10] = LIN_Checksum(&frame[0], 8);

    //HAL_GPIO_WritePin(CS_GPIO_Port , CS_Pin, GPIO_PIN_SET);


    /* Send LIN break (13-bit low) */
    HAL_LIN_SendBreak(&huart1);
    while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_TC) == RESET);
    HAL_UART_Transmit(&huart1, &frame[0], 11, HAL_MAX_DELAY);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2)
    {
        /* no can packets ≥ 300 000 ms -> to Stop */
        if ((HAL_GetTick() - lastCanRx) >= 150000U)
    	//if ((HAL_GetTick() - lastCanRx) >= 30000000U)
        {
        	sleep_flag = 1;
        }
    }
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CanFrame *f = &rxBuffer[rxHead];
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0,  &f->header, f->data) != HAL_OK) {
        Error_Handler();
    }
    rxHead = (rxHead + 1) % CAN_RX_BUFFER_SIZE;
    if (rxHead == rxTail) {
        // buffer overflow – delete the oldest one value
        rxTail = (rxTail + 1) % CAN_RX_BUFFER_SIZE;
    }
    lastCanRx = HAL_GetTick();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CAN_Init();
  MX_USART1_UART_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  if (HAL_CAN_Start(&hcan) != HAL_OK) {
      Error_Handler();
  }
  if (HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) {
      Error_Handler();
  }
  Enable_CAN();
  HAL_TIM_Base_Start_IT(&htim2);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
	  processCanFrames();

      if (sleep_flag > 0) {
              enter_stop_and_resume();
              sleep_flag = 0;
      }
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
