/*
 * Aplicação Zephyr RTOS - Passo 1: LED Blink
 * Versão simplificada para STM32F429I Discovery
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED Configuration - LED3 (Verde) da placa STM32F429I Discovery */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Thread configuration */
#define LED_STACK_SIZE 512
#define LED_PRIORITY 5

/* Thread stack */
K_THREAD_STACK_DEFINE(led_stack, LED_STACK_SIZE);
struct k_thread led_thread_data;

/* LED Task */
void led_task(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    LOG_INF("LED task started");

    while (1) {
        /* Toggle LED */
        gpio_pin_toggle_dt(&led);
        LOG_INF("LED toggled");
        
        /* Sleep for 500ms */
        k_sleep(K_MSEC(500));
    }
}

/* Main function */
int main(void)
{
    int ret;

    LOG_INF("Zephyr LED Blink Application Starting...");

    /* Check if LED device is ready */
    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    /* Configure LED pin as output */
    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin: %d", ret);
        return ret;
    }

    /* Create LED thread */
    k_thread_create(&led_thread_data, led_stack,
                    K_THREAD_STACK_SIZEOF(led_stack),
                    led_task, NULL, NULL, NULL,
                    LED_PRIORITY, 0, K_NO_WAIT);
    
    k_thread_name_set(&led_thread_data, "led_task");

    LOG_INF("LED task created successfully");

    /* Main thread just sleeps */
    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}