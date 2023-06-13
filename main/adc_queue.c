#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "rtc_wdt.h"

// FW Configuration
#define SAMPLING_PERIOD_US 100           // 100 us = 0.1ms
#define DATA_RETENTION_PERIOD_US 1000000 // 1s
#define LED_GPIO 2
// End of FW Configuration

static const unsigned int dataCount = (int)ceil(DATA_RETENTION_PERIOD_US / SAMPLING_PERIOD_US);

static esp_adc_cal_characteristics_t adc1_chars;
static SemaphoreHandle_t timer_sem;
static SemaphoreHandle_t xSemaphore;
static SemaphoreHandle_t xMutex;
static QueueHandle_t queue;
static QueueHandle_t queue2;

void setup_adc(void)
{
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_9, 0, &adc1_chars);

    adc1_config_width(ADC_WIDTH_BIT_9);
    adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);
}

void setup_led(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
}

void timer_callback(void *pvParameter)
{
    xSemaphoreGiveFromISR(timer_sem, NULL);
}

void read_adc_conf(void *pvParameter)
{
    // configure watchdog timer so that it will not reset the system
    esp_task_wdt_config_t config = {
        .timeout_ms = 5000,  // Timeout in milliseconds
        .idle_core_mask = 0,  // Check idle task
        .trigger_panic = false  // Enable task watchdog
        // Add more configuration settings as needed
    };

    // Initialize and start the watchdog timer
    esp_task_wdt_deinit();
    esp_task_wdt_init(&config);

    // initialize pointer from pvParams
    unsigned long int *adcDataSum = (unsigned long int *)pvParameter;
    // initialize counter to count how many data has been read
    unsigned int counter = 0;

    // initialize timer to interrupt every 100 us
    timer_sem = xSemaphoreCreateBinary();
    const esp_timer_create_args_t my_timer_args = {
        .callback = &timer_callback,
        .name = "ISR Timer"};
    esp_timer_handle_t timer_handler;
    ESP_ERROR_CHECK(esp_timer_create(&my_timer_args, &timer_handler));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handler, 100));

    while (1)
    {
        // take semaphore from timer ISR
        xSemaphoreTake(timer_sem, portMAX_DELAY);
        // read adc value
        int adc_value = adc1_get_raw(ADC1_CHANNEL_4);
        // add adc value to pointer with mutex
        if(xSemaphoreTake(xMutex, portMAX_DELAY)) *adcDataSum += adc_value;
        xSemaphoreGive(xMutex);
        // increment counter
        counter++;
        // if counter reaches dataCount, reset counter and give semaphore to task 2
        if (counter == dataCount)
        {
            counter = 0;
            xSemaphoreGive(xSemaphore);
        }
    }
}

void calc_ave_adc(void *pvParameter)
{
    // initialize queue
    char txBuffer[50];
    int txBuffer2;
    queue = xQueueCreate(5, sizeof(txBuffer));
    queue2 = xQueueCreate(5, sizeof(txBuffer2));
    if (queue == 0 || queue2 == 0)
    {
        printf("Failed to create queue");
    }

    // initialize pointer from pvParams
    unsigned long int *adcDataSum = (unsigned long int *)pvParameter;

    while (1)
    {
        if (xSemaphoreTake(xSemaphore, portMAX_DELAY))
        {
            // initialize variable datasum
            unsigned long int dataSum = 0;
            
            // to avoid conflict, use mutex
            if(xSemaphoreTake(xMutex, portMAX_DELAY)) {
                // copy data from pointer to datasum variable
                dataSum = *adcDataSum;
                // reset pointer to zero, indicating new cycle begins
                *adcDataSum = 0;
            }
            // give back the mutex so task 1 can run again
            xSemaphoreGive(xMutex);

            // calculate average data using copied data
            double aveData = (double)dataSum / (double)dataCount;

            // send data to queue
            txBuffer2 = (int)round(aveData);
            xQueueSend(queue2, (void *)&txBuffer2, (TickType_t)0);
            sprintf(txBuffer, "%.3lf", aveData);
            xQueueSend(queue, (void *)txBuffer, (TickType_t)0);

            // delay task so another task can run in the meantime
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }
}

void print_ave_adc(void *pvParameters)
{
    // initialize queue
    char rxBuffer[50];
    while (1)
    {
        // receive data from queue
        if (xQueueReceive(queue, &(rxBuffer), (TickType_t)10) == pdPASS)
        {
            // print data
            printf("Received average data from queue == %s\n", rxBuffer);
        }
        // delay task so another task can run in the meantime
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

void led_control(void *pvParameters)
{
    // initialize queue
    int rxBuffer2;
    while (1)
    {
        // receive data from queue
        if (xQueueReceive(queue2,  &(rxBuffer2), (TickType_t)10) == pdPASS) {
            // control led
            if (rxBuffer2 > 255) {
                gpio_set_level(LED_GPIO, 1);
            } else {
                gpio_set_level(LED_GPIO, 0);
            }
        }
    }
}

void app_main(void)
{

    setup_adc();
    setup_led();

    BaseType_t xReturned;
    TaskHandle_t xHandle1 = NULL;
    TaskHandle_t xHandle2 = NULL;
    TaskHandle_t xHandle3 = NULL;
    TaskHandle_t xHandle4 = NULL;

    xSemaphore = xSemaphoreCreateBinary();
    xMutex = xSemaphoreCreateMutex();

    unsigned long int adcDataSum = 0;
    bool ledState = false;
    
    xReturned = xTaskCreatePinnedToCore(
        read_adc_conf,      // Function that implements the task.
        "Read ADC",         // Text name for the task.
        50000,              // Stack size in words, not bytes.
        (void *) &adcDataSum, // Parameter passed into the task.
        1,                  // Priority at which the task is created.
        &xHandle1,           // Used to pass out the created task's handle.
        0);                 // Core 0

    xReturned = xTaskCreatePinnedToCore(
        calc_ave_adc,            // Function that implements the task.
        "Calculate Average ADC", // Text name for the task.
        10000,                   // Stack size in words, not bytes.
        (void *) &adcDataSum,      // Parameter passed into the task.
        1,                       // Priority at which the task is created.
        &xHandle2,
        1); // Used to pass out the created task's handle.
    
    xReturned = xTaskCreatePinnedToCore(
        print_ave_adc,            // Function that implements the task.
        "Print Average ADC", // Text name for the task.
        10000,                   // Stack size in words, not bytes.
        NULL,      // Parameter passed into the task.
        3,                       // Priority at which the task is created.
        &xHandle3,
        1); // Used to pass out the created task's handle.
    
    xReturned = xTaskCreatePinnedToCore(
        led_control,            // Function that implements the task.
        "LED Control", // Text name for the task.
        10000,                   // Stack size in words, not bytes.
        NULL,      // Parameter passed into the task.
        4,                       // Priority at which the task is created.
        &xHandle3,
        1); // Used to pass out the created task's handle.
}
