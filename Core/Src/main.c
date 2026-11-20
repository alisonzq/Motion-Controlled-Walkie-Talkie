/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "wifi.h"        /* SPI_WIFI_ISR() for the PE1 interrupt */
#include "wifi_link.h"   /* audio link between the boards: wifi_link_init, _send_frame, _poll, ... */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// Wi-Fi test mode, until the microphone and speaker are connected to wifi_link:
//   WIFI_TEST_TX     = send dummy frames to PEER_IP
//   WIFI_TEST_RX     = receive and print stats
//   WIFI_TEST_SWITCH = switch RX <-> TX every 5 s (tests mode switching with one board + laptop)
#define WIFI_TEST_TX 0
#define WIFI_TEST_RX 1
#define WIFI_TEST_SWITCH 2
#define WIFI_TEST_MODE WIFI_TEST_SWITCH
#define WIFI_TEST_SWITCH_PERIOD_MS 5000
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
DFSDM_Filter_HandleTypeDef hdfsdm1_filter0;
DFSDM_Channel_HandleTypeDef hdfsdm1_channel2;
DMA_HandleTypeDef hdma_dfsdm1_flt0;

I2C_HandleTypeDef hi2c2;

SPI_HandleTypeDef hspi3;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
// state machine to show whether the walkie-talkie is receiving or transmitting
static enum {
	//receiving
	RECEIVING,
	//transmitting
	TRANSMITTING
} state ;

int32_t microphone_buffer[64] ;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_I2C2_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_SPI3_Init(void);
static void MX_DFSDM1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void log_uart(const char *s)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 100);
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
  MX_DMA_Init();
  MX_I2C2_Init();
  MX_USART1_UART_Init();
  MX_SPI3_Init();
  MX_DFSDM1_Init();
  /* USER CODE BEGIN 2 */
  char msg[128];
  uint8_t ip[4];

  log_uart("Wi-Fi init...\r\n");
  WifiLinkStatus wl = wifi_link_init(); // starts in RX mode
  if (wl != WIFI_LINK_OK) {
    snprintf(msg, sizeof msg, "Wi-Fi FAILED (step %d), module replied: %.50s\r\n",
             (int)wl, wifi_link_last_error());
    log_uart(msg);
    Error_Handler();
  }
  wifi_link_get_ip(ip);
  snprintf(msg, sizeof msg, "Wi-Fi ready, IP %d.%d.%d.%d\r\n", ip[0], ip[1], ip[2], ip[3]);
  log_uart(msg);
  log_uart("Looking for the other board...\r\n");
  if (wifi_link_find_peer(30000) == WIFI_LINK_OK) { // up to 30 s
    wifi_link_get_peer_ip(ip);
    snprintf(msg, sizeof msg, "Found the other board: %d.%d.%d.%d\r\n", ip[0], ip[1], ip[2], ip[3]);
  } else {
    snprintf(msg, sizeof msg, "No other board found (TX will fail until it appears)\r\n");
  }
  log_uart(msg);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET); // LED2 on = connected

  static int16_t frame[WIFI_LINK_FRAME_SAMPLES];
  uint32_t last_report = HAL_GetTick();
  uint32_t last_switch = HAL_GetTick();
  (void)last_switch; // only used in the SWITCH test
  WifiLinkStats st, prev = {0};

  for (int i = 0; i < WIFI_LINK_FRAME_SAMPLES; i++) {
    frame[i] = (int16_t)(i * 100); // dummy ramp, replaced by mic frames later
  }

#if WIFI_TEST_MODE == WIFI_TEST_TX
  wl = wifi_link_set_mode(WIFI_LINK_MODE_TX);
  if (wl != WIFI_LINK_OK) {
    snprintf(msg, sizeof msg, "TX mode FAILED (step %d): %.50s\r\n", (int)wl, wifi_link_last_error());
    log_uart(msg);
    Error_Handler();
  }
  log_uart("TX test: sending frames...\r\n");
#elif WIFI_TEST_MODE == WIFI_TEST_RX
  log_uart("RX test: waiting for frames...\r\n");
#else
  log_uart("SWITCH test: RX and TX alternate every 5 s\r\n");
#endif
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // Keep the Wi-Fi link alive (rejoins the hotspot if it dropped us)
    WifiLinkWatchdog wd = wifi_link_watchdog();
    if (wd == WIFI_LINK_WD_RECONNECTED) {
      uint8_t peer[4];
      wifi_link_get_ip(ip);
      wifi_link_get_peer_ip(peer);
      snprintf(msg, sizeof msg, "!! Wi-Fi link lost and back: IP %d.%d.%d.%d, peer %d.%d.%d.%d, mode %s\r\n",
               ip[0], ip[1], ip[2], ip[3], peer[0], peer[1], peer[2], peer[3],
               wifi_link_get_mode() == WIFI_LINK_MODE_TX ? "TX" : "RX");
      log_uart(msg);
    } else if (wd == WIFI_LINK_WD_DOWN) {
      snprintf(msg, sizeof msg, "!! Wi-Fi link lost, rejoin failed (%.40s), retrying in 3 s\r\n", wifi_link_last_error());
      log_uart(msg);
    }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, wifi_link_is_up() ? GPIO_PIN_SET : GPIO_PIN_RESET); // LED2 = link up

#if WIFI_TEST_MODE == WIFI_TEST_SWITCH
    if (wifi_link_is_up() && HAL_GetTick() - last_switch >= WIFI_TEST_SWITCH_PERIOD_MS) { // time to switch mode
      WifiLinkMode next = (wifi_link_get_mode() == WIFI_LINK_MODE_RX) ? WIFI_LINK_MODE_TX : WIFI_LINK_MODE_RX;
      uint32_t t0 = HAL_GetTick();
      wl = wifi_link_set_mode(next);
      uint32_t took = HAL_GetTick() - t0;
      if (wl != WIFI_LINK_OK) {
        snprintf(msg, sizeof msg, "switch FAILED (step %d): %.50s\r\n", (int)wl, wifi_link_last_error());
      } else {
        uint32_t close_ms, open_ms;
        wifi_link_get_switch_timing(&close_ms, &open_ms);
        snprintf(msg, sizeof msg, "---- switched to %s in %lu ms (close %s %lu ms, open %s %lu ms) ----\r\n",
                 next == WIFI_LINK_MODE_TX ? "TX" : "RX", took,
                 next == WIFI_LINK_MODE_TX ? "RX" : "TX", close_ms,
                 next == WIFI_LINK_MODE_TX ? "TX" : "RX", open_ms);
      }
      log_uart(msg);
      last_switch = HAL_GetTick();
    }
#endif

    if (wifi_link_get_mode() == WIFI_LINK_MODE_TX) {
      wifi_link_send_frame(frame);
      HAL_Delay(20); // about 50 frames/s, the audio rate
    } else {
      wifi_link_poll(frame, 10); // frame now holds the received audio
    }

    if (HAL_GetTick() - last_report >= 1000) { // stats once per second
      int tx = (wifi_link_get_mode() == WIFI_LINK_MODE_TX);
      wifi_link_get_stats(&st);
      snprintf(msg, sizeof msg, "%s %lu/s  sent %lu (err %lu)  rx %lu  lost %lu  bad %lu  flushed %lu  reconn %lu\r\n",
               tx ? "TX" : "RX",
               tx ? st.sent - prev.sent : st.received - prev.received,
               st.sent, st.send_errors, st.received, st.lost, st.bad, st.flushed,
               st.reconnects);
      log_uart(msg);
      prev = st;
      last_report += 1000;
    }
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

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 60;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief DFSDM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_DFSDM1_Init(void)
{

  /* USER CODE BEGIN DFSDM1_Init 0 */

  /* USER CODE END DFSDM1_Init 0 */

  /* USER CODE BEGIN DFSDM1_Init 1 */

  /* USER CODE END DFSDM1_Init 1 */
  hdfsdm1_filter0.Instance = DFSDM1_Filter0;
  hdfsdm1_filter0.Init.RegularParam.Trigger = DFSDM_FILTER_SW_TRIGGER;
  hdfsdm1_filter0.Init.RegularParam.FastMode = ENABLE;
  hdfsdm1_filter0.Init.RegularParam.DmaMode = ENABLE;
  hdfsdm1_filter0.Init.FilterParam.SincOrder = DFSDM_FILTER_FASTSINC_ORDER;
  hdfsdm1_filter0.Init.FilterParam.Oversampling = 250;
  hdfsdm1_filter0.Init.FilterParam.IntOversampling = 1;
  if (HAL_DFSDM_FilterInit(&hdfsdm1_filter0) != HAL_OK)
  {
    Error_Handler();
  }
  hdfsdm1_channel2.Instance = DFSDM1_Channel2;
  hdfsdm1_channel2.Init.OutputClock.Activation = ENABLE;
  hdfsdm1_channel2.Init.OutputClock.Selection = DFSDM_CHANNEL_OUTPUT_CLOCK_SYSTEM;
  hdfsdm1_channel2.Init.OutputClock.Divider = 60;
  hdfsdm1_channel2.Init.Input.Multiplexer = DFSDM_CHANNEL_EXTERNAL_INPUTS;
  hdfsdm1_channel2.Init.Input.DataPacking = DFSDM_CHANNEL_STANDARD_MODE;
  hdfsdm1_channel2.Init.Input.Pins = DFSDM_CHANNEL_SAME_CHANNEL_PINS;
  hdfsdm1_channel2.Init.SerialInterface.Type = DFSDM_CHANNEL_SPI_RISING;
  hdfsdm1_channel2.Init.SerialInterface.SpiClock = DFSDM_CHANNEL_SPI_CLOCK_INTERNAL;
  hdfsdm1_channel2.Init.Awd.FilterOrder = DFSDM_CHANNEL_FASTSINC_ORDER;
  hdfsdm1_channel2.Init.Awd.Oversampling = 1;
  hdfsdm1_channel2.Init.Offset = 0;
  hdfsdm1_channel2.Init.RightBitShift = 0x00;
  if (HAL_DFSDM_ChannelInit(&hdfsdm1_channel2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DFSDM_FilterConfigRegChannel(&hdfsdm1_filter0, DFSDM_CHANNEL_2, DFSDM_CONTINUOUS_CONV_ON) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DFSDM1_Init 2 */

  /* USER CODE END DFSDM1_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x30A175AB;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief SPI3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI3_Init(void)
{

  /* USER CODE BEGIN SPI3_Init 0 */

  /* USER CODE END SPI3_Init 0 */

  /* USER CODE BEGIN SPI3_Init 1 */

  /* USER CODE END SPI3_Init 1 */
  /* SPI3 parameter configuration*/
  hspi3.Instance = SPI3;
  hspi3.Init.Mode = SPI_MODE_MASTER;
  hspi3.Init.Direction = SPI_DIRECTION_2LINES;
  hspi3.Init.DataSize = SPI_DATASIZE_4BIT;
  hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi3.Init.NSS = SPI_NSS_SOFT;
  hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi3.Init.CRCPolynomial = 7;
  hspi3.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi3.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
  if (HAL_SPI_Init(&hspi3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI3_Init 2 */

  /* USER CODE END SPI3_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */

/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(WIFI_RST_GPIO_Port, WIFI_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, WIFI_BOOT0_Pin|WIFI_WAKEUP_Pin|led_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(BLE_CS_GPIO_Port, BLE_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(WIFI_CS_GPIO_Port, WIFI_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : btn_Pin */
  GPIO_InitStruct.Pin = btn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(btn_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : WIFI_RST_Pin WIFI_CS_Pin */
  GPIO_InitStruct.Pin = WIFI_RST_Pin|WIFI_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pins : WIFI_BOOT0_Pin WIFI_WAKEUP_Pin led_Pin */
  GPIO_InitStruct.Pin = WIFI_BOOT0_Pin|WIFI_WAKEUP_Pin|led_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : BLE_CS_Pin */
  GPIO_InitStruct.Pin = BLE_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BLE_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PE1 */
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

/* USER CODE BEGIN MX_GPIO_Init_2 */

/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* Wi-Fi module: SPI3 interrupt. */
extern SPI_HandleTypeDef hspi;
void SPI3_IRQHandler(void)
{
  HAL_SPI_IRQHandler(&hspi);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_1) /* PE1: Wi-Fi module data ready */
  {
    SPI_WIFI_ISR();
  }
  else if (GPIO_Pin == GPIO_PIN_13) /* PC13: blue push-button */
  {
		HAL_GPIO_TogglePin(led_GPIO_Port, led_Pin);
		switch(state){
		case RECEIVING:
			if (HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0,microphone_buffer,64)!= HAL_OK){
				Error_Handler();
			}
			state = TRANSMITTING;
			break;
		case TRANSMITTING:
			if (HAL_DFSDM_FilterRegularStop_DMA(&hdfsdm1_filter0)!= HAL_OK){
				Error_Handler();
			}
			state = RECEIVING;
			break;
		}
  }
}
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
