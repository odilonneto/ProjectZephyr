#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h> // Inclui a API do Shell
#include <stdlib.h>             // Para atoi, etc. (similar ao exemplo)
#include <string.h>             // Para strcmp (similar ao exemplo)

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* --- CONFIGURAÇÃO DO LED E THREAD --- */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define LED_STACK_SIZE 512
#define LED_PRIORITY 5 // Prioridade da tarefa do LED

K_THREAD_STACK_DEFINE(led_stack, LED_STACK_SIZE);
struct k_thread led_thread_data;
k_tid_t led_thread_id; // Vamos guardar o ID da thread para controlá-la

// Flag para monitorar o estado da nossa tarefa de LED
static bool led_task_running = true;

/* --- TAREFA DO LED (Soft Real-Time) --- */
// Esta é um exemplo de tarefa de tempo real soft.
void led_task(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    LOG_INF("LED task started");

    while (1) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
}

/* --- COMANDOS DO SHELL --- */

/* Comando para controlar a tarefa do LED */
static int cmd_led_control(const struct shell *shell, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_print(shell, "Uso: led_control <start|stop>");
        return -EINVAL;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (!led_task_running) {
            k_thread_resume(led_thread_id);
            led_task_running = true;
            shell_print(shell, "Tarefa do LED reiniciada.");
        } else {
            shell_print(shell, "Tarefa do LED ja esta em execucao.");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        if (led_task_running) {
            k_thread_suspend(led_thread_id);
            led_task_running = false;
            // Opcional: garantir que o LED apague ao parar
            gpio_pin_set_dt(&led, 0);
            shell_print(shell, "Tarefa do LED suspensa.");
        } else {
            shell_print(shell, "Tarefa do LED ja esta suspensa.");
        }
    } else {
        shell_print(shell, "Comando invalido. Use 'start' ou 'stop'.");
        return -EINVAL;
    }

    return 0;
}

/* Comando para exibir informações da tarefa do LED */
static int cmd_task_info(const struct shell *shell, size_t argc, char **argv)
{
    shell_print(shell, "--- Informacoes da Tarefa de Tempo Real (LED) ---");
    shell_print(shell, "Estado: %s", led_task_running ? "Executando" : "Suspensa");
    shell_print(shell, "Prioridade: %d", LED_PRIORITY);
    
    // Para informações mais detalhadas, o Zephyr já fornece um comando
    shell_print(shell, "Para detalhes de runtime (CPU, stack), use o comando: 'kernel threads'");

    return 0;
}


/* Registro dos comandos no sistema do Shell */
SHELL_CMD_REGISTER(led_control, NULL, "Controla a tarefa do LED (start/stop)", cmd_led_control);
SHELL_CMD_REGISTER(task_info, NULL, "Mostra informacoes da tarefa do LED", cmd_task_info);


/* --- FUNÇÃO PRINCIPAL --- */
int main(void)
{
    int ret;

    LOG_INF("Aplicacao Zephyr com Shell Iniciada...");

    /* Verifica se o dispositivo do LED está pronto */
    if (!device_is_ready(led.port)) {
        LOG_ERR("Dispositivo do LED nao esta pronto");
        return -ENODEV;
    }

    /* Configura o pino do LED como saída */
    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Falha ao configurar o pino do LED: %d", ret);
        return ret;
    }

    /* Cria a thread do LED */
    led_thread_id = k_thread_create(&led_thread_data, led_stack,
                    K_THREAD_STACK_SIZEOF(led_stack),
                    led_task, NULL, NULL, NULL,
                    LED_PRIORITY, 0, K_NO_WAIT);
    
    // Define um nome para a thread, que aparecerá nos comandos do shell
    k_thread_name_set(led_thread_id, "led_task");

    LOG_INF("Tarefa do LED criada com sucesso. Shell esta pronto.");

    // A thread principal não precisa fazer mais nada.
    // O shell e a tarefa do LED rodam em seus próprios contextos.
    return 0;
}