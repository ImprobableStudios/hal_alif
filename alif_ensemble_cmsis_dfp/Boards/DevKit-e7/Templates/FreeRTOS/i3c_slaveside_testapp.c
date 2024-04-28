/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/**************************************************************************//**
 * @file     i3c_slaveside_testapp.c
 * @author   Prabhakar kumar
 * @email    prabhakar.kumar@alifsemi.com
 * @version  V1.0.0
 * @date     27-May-2023
 * @brief    Baremetal to verify slave side loop back test
 *
 *           Slave receives n-bytes of data from Master
 *            and sends back received data again to Master.
 *
 *           I3C slave configuration.
 *           - In control API parameter:
 *             control : I3C_SET_SLAVE_ADDR(set slave address)
 *             arg     : I3C_SLAVE_ADDRESS macro is defined to set address
 *                       of slave
 *
 *           Hardware Setup:
 *            Required two boards one for Master and one for Slave
 *             (as there is only one i3c instance is available on ASIC).
 *
 *           Connect SDA to SDA and SCL to SCL and GND to GND.
 *            - SDA P7_6 -> SDA P7_6
 *            - SCL P7_7 -> SCL P7_7
 *            - GND      -> GND
 * @bug      None.
 * @Note     None.
 ******************************************************************************/

/* System Includes */
#include <stdio.h>
#include "string.h"
#include "system_utils.h"

/* Project Includes */
#include "Driver_I3C.h"

/* PINMUX Driver */
#include "pinconf.h"
#include "Driver_GPIO.h"

/* Rtos include */
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"

#include "RTE_Components.h"
#if defined(RTE_Compiler_IO_STDOUT)
#include "retarget_stdout.h"
#endif  /* RTE_Compiler_IO_STDOUT */


/* i3c Driver instance 0 */
extern ARM_DRIVER_I3C Driver_I3C;
static ARM_DRIVER_I3C *I3Cdrv = &Driver_I3C;

/*Define for FreeRTOS*/
#define STACK_SIZE     1024
#define TIMER_SERVICE_TASK_STACK_SIZE configTIMER_TASK_STACK_DEPTH
#define IDLE_TASK_STACK_SIZE          configMINIMAL_STACK_SIZE

StackType_t IdleStack[2 * IDLE_TASK_STACK_SIZE];
StaticTask_t IdleTcb;
StackType_t TimerStack[2 * TIMER_SERVICE_TASK_STACK_SIZE];
StaticTask_t TimerTcb;

TaskHandle_t i3c_xHandle;

/****************************** FreeRTOS functions **********************/

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
      StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize)
{
   *ppxIdleTaskTCBBuffer = &IdleTcb;
   *ppxIdleTaskStackBuffer = IdleStack;
   *pulIdleTaskStackSize = IDLE_TASK_STACK_SIZE;
}

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
   (void) pxTask;

   for (;;);
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
      StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize)
{
   *ppxTimerTaskTCBBuffer = &TimerTcb;
   *ppxTimerTaskStackBuffer = TimerStack;
   *pulTimerTaskStackSize = TIMER_SERVICE_TASK_STACK_SIZE;
}

void vApplicationIdleHook(void)
{
   for (;;);
}

/* set slave address */
#define I3C_SLAVE_ADDRESS             (0X48)

/* receive data from i3c */
uint8_t rx_data[4] = {0x00};

uint32_t tx_cnt = 0;
uint32_t rx_cnt = 0;

void i3c_slave_loopback_thread(void *pvParameters);

/* i3c callback events */
typedef enum _I3C_CB_EVENT{
    I3C_CB_EVENT_SUCCESS            = (1 << 0),
    I3C_CB_EVENT_ERROR              = (1 << 1)
}I3C_CB_EVENT;

/**
  \fn          int32_t hardware_init(void)
  \brief       i3c hardware pin initialization:
                - PIN-MUX configuration
                - PIN-PAD configuration
  \param[in]   void
  \return      0:success; -1:failure
*/
int32_t hardware_init(void)
{
    /* for I3C_D(PORT_7 PIN_6(SDA)/PIN_7(SCL)) instance,
     *  for I3C in I3C mode (not required for I3C in I2C mode)
     *  GPIO voltage level(flex) has to be change to 1.8-V power supply.
     *
     *  GPIO_CTRL Register field VOLT:
     *   Select voltage level for the 1.8-V/3.3-V (flex) I/O pins
     *    0x0: I/O pin will be used with a 3.3-V power supply
     *    0x1: I/O pin will be used with a 1.8-V power supply
     */

    /* Configure GPIO flex I/O pins to 1.8-V:
     *  P7_6 and P7_7 pins are part of GPIO flex I/O pins,
     *   so we can use any one of the pin to configure flex I/O.
     */
#define GPIO7_PORT          7

    extern  ARM_DRIVER_GPIO ARM_Driver_GPIO_(GPIO7_PORT);
    ARM_DRIVER_GPIO *gpioDrv = &ARM_Driver_GPIO_(GPIO7_PORT);

    int32_t  ret = 0;
    uint32_t arg = 0;

    ret = gpioDrv->Initialize(PIN_6, NULL);
    if (ret != 0)
    {
        printf("ERROR: Failed to initialize GPIO \n");
        return -1;
    }

    ret = gpioDrv->PowerControl(PIN_6, ARM_POWER_FULL);
    if (ret != 0)
    {
        printf("ERROR: Failed to powered full GPIO \n");
        return -1;
    }

    /* select control argument as flex 1.8-V */
    arg = ARM_GPIO_FLEXIO_VOLT_1V8;
    ret = gpioDrv->Control(PIN_6, ARM_GPIO_CONFIG_FLEXIO, &arg);
    if (ret != ARM_DRIVER_OK)
    {
        printf("ERROR: Failed to control GPIO Flex \n");
        return -1;
    }

    /* I3C_SDA_D */
    ret = pinconf_set(PORT_7, PIN_6, PINMUX_ALTERNATE_FUNCTION_6,
                PADCTRL_READ_ENABLE | PADCTRL_DRIVER_DISABLED_PULL_UP |
                PADCTRL_OUTPUT_DRIVE_STRENGTH_4MA);

    /* I3C_SCL_D */
    ret = pinconf_set(PORT_7, PIN_7, PINMUX_ALTERNATE_FUNCTION_6,
                PADCTRL_READ_ENABLE | PADCTRL_DRIVER_DISABLED_PULL_UP |
                PADCTRL_OUTPUT_DRIVE_STRENGTH_4MA);

    return ret;
}

/**
  \fn          void I3C_callback(UINT event)
  \brief       i3c isr callback
  \param[in]   event: i3c Event
  \return      none
*/
static void I3C_callback(uint32_t event)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE, xResult = pdFALSE;

    if (event & ARM_I3C_EVENT_TRANSFER_DONE)
    {
        /* Transfer Success */
        xResult = xTaskNotifyFromISR(i3c_xHandle, I3C_CB_EVENT_SUCCESS,
                                     eSetBits, &xHigherPriorityTaskWoken);
        if (xResult == pdTRUE)
        {
            portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
        }
    }
    if (event & ARM_I3C_EVENT_TRANSFER_ERROR)
    {
        /* Transfer Error */
        xResult = xTaskNotifyFromISR(i3c_xHandle, I3C_CB_EVENT_ERROR,
                                     eSetBits, &xHigherPriorityTaskWoken);
        if (xResult == pdTRUE)
        {
            portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
        }
    }
}

/**
  \fn          void i3c_slave_loopback_thread(void *pvParameters)
  \brief       Baremetal to verify i3c slave mode
               This demo does:
                 - initialize i3c driver;
                 - set slave address and initialize slave
                 - Receive and transmit byte from master
  \return      none
*/
void i3c_slave_loopback_thread(void *pvParameters)
{
    int   ret    = 0;
    int   len    = 0;
    uint32_t actual_events = 0;

    ARM_DRIVER_VERSION version;

    printf("\r\n \t\t >>> Slave loop back demo starting up!!! <<< \r\n");

    /* Get i3c driver version. */
    version = I3Cdrv->GetVersion();
    printf("\r\n i3c version api:0x%X driver:0x%X \r\n",
                           version.api, version.drv);

    /* Initialize i3c hardware pins using PinMux Driver. */
    ret = hardware_init();
    if(ret != 0)
    {
        printf("\r\n Error: i3c hardware_init failed.\r\n");
        return;
    }

    /* Initialize I3C driver */
    ret = I3Cdrv->Initialize(I3C_callback);
    if(ret != ARM_DRIVER_OK)
    {
        printf("\r\n Error: I3C Initialize failed.\r\n");
        return;
    }

    /* Power up I3C peripheral */
    ret = I3Cdrv->PowerControl(ARM_POWER_FULL);
    if(ret != ARM_DRIVER_OK)
    {
        printf("\r\n Error: I3C Power Up failed.\r\n");
        goto error_uninitialize;
    }

    /* Control I3C interface */
    ret = I3Cdrv->Control(I3C_SLAVE_SET_ADDR, I3C_SLAVE_ADDRESS);
    if(ret != ARM_DRIVER_OK)
    {
        printf("\r\n Error: I3C Control failed.\r\n");
        goto error_poweroff;
    }


    while(1)
    {
        len = 4;

        /* Slave Receive */
        ret = I3Cdrv->SlaveReceive(rx_data, len);
        if(ret != ARM_DRIVER_OK)
        {
            printf("\r\n Error: I3C Slave Receive failed. \r\n");
            goto error_poweroff;
        }

        /* wait for callback event. */
        xTaskNotifyWait(NULL, I3C_CB_EVENT_SUCCESS | I3C_CB_EVENT_ERROR,
                        &actual_events, portMAX_DELAY);

        if(actual_events == I3C_CB_EVENT_ERROR)
        {
            printf("\nError: I3C Slave Receive failed\n");
            while(1);
        }

        rx_cnt += 1;

        /* For loop back test,
         * Slave will send received data back to Master.
         */

        /* Slave Transmit*/
        ret = I3Cdrv->SlaveTransmit(rx_data, len);
        if(ret != ARM_DRIVER_OK)
        {
            printf("\r\n Error: I3C slave Transmit failed. \r\n");
            goto error_poweroff;
        }

        /* wait for callback event. */
        xTaskNotifyWait(NULL, I3C_CB_EVENT_SUCCESS | I3C_CB_EVENT_ERROR,
                        &actual_events, portMAX_DELAY);

        if(actual_events == I3C_CB_EVENT_ERROR)
        {
            printf("\nError: I2C Slave Transmit failed\n");
            while(1);
        }

        tx_cnt += 1;
    }

error_poweroff:

    /* Power off I3C peripheral */
    ret = I3Cdrv->PowerControl(ARM_POWER_OFF);
    if(ret != ARM_DRIVER_OK)
    {
         printf("\r\n Error: I3C Power OFF failed.\r\n");
    }

error_uninitialize:

    /* Un-initialize I3C driver */
    ret = I3Cdrv->Uninitialize();
    if(ret != ARM_DRIVER_OK)
    {
        printf("\r\n Error: I3C Uninitialize failed.\r\n");
    }

    printf("\r\n I3C demo exiting ...\r\n");

    /* thread delete */
    vTaskDelete( NULL );
}


/*----------------------------------------------------------------------------
 *      Main: Initialize and start the FreeRTOS Kernel
 *---------------------------------------------------------------------------*/
int main(void)
{
#if defined(RTE_Compiler_IO_STDOUT_User)
    int32_t ret;
    ret = stdout_init();
    if(ret != ARM_DRIVER_OK)
    {
        while(1)
        {
        }
    }
#endif

   /* System Initialization */
   SystemCoreClockUpdate();

   /* Create application main thread */
   BaseType_t xReturned = xTaskCreate(i3c_slave_loopback_thread, "i3c_slave_loopback_thread",
                                      256, NULL,configMAX_PRIORITIES-1, &i3c_xHandle);
   if (xReturned != pdPASS)
   {
      vTaskDelete(i3c_xHandle);
      return -1;
   }

   /* Start thread execution */
   vTaskStartScheduler();
}

/************************ (C) COPYRIGHT ALIF SEMICONDUCTOR *****END OF FILE****/
