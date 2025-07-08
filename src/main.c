#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h> // api do shel, comunicação
#include <stdlib.h>             // para atoi
#include <string.h>             // para strlen, strtol
#include <zephyr/debug/thread_analyzer.h>


LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* --- CONFIGURAÇÃO DOS LEDS, BOTÕES E THREADS --- */
#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios,
							      {0});
static struct gpio_callback button_cb_data;
static uint64_t last_total_cycles = 0;
static uint64_t last_idle_cycles = 0;
volatile uint32_t led_speed = 1000;
volatile uint8_t led_mode = 0; // 0 = leds alternando, 1 = apenas led verde, 2 = apenas led vermelho, 3 = leds sincronizados
#define LED_STACK_SIZE 512
#define LED_PRIORITY 5 // Prioridade da tarefa do LED

K_THREAD_STACK_DEFINE(led_stack, LED_STACK_SIZE);
struct k_thread led_thread_data;
k_tid_t led_thread_id;

/* --- TAREFA DO LED (Soft Real-Time) --- */
// Esta é um exemplo de tarefa de tempo real soft.
/* Esta tarefa pisca os leds de acordo com o led_mode, definido no cabeçalho do arquivo. */
void led_task(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    LOG_INF("LED task started");
    gpio_pin_toggle_dt(&led); // inicia a task como led_mode = 0;

    while (1) {

        if (led_mode == 0 || led_mode == 3){
            gpio_pin_toggle_dt(&led);
            gpio_pin_toggle_dt(&led1);
        } else if (led_mode == 1){
            gpio_pin_toggle_dt(&led);
        } else if (led_mode == 2){
            gpio_pin_toggle_dt(&led1);
        }

        k_sleep(K_MSEC(led_speed));
    }
}

/* Função axuiliar que verifica se uma string pode ser convertida a um número
    Retorna 1 se sim, 0 se não.
*/
int is_string_number(const char *str) {
    char *endptr;
    long val = strtol(str, &endptr, 10);
    (void*) val;
    return (*endptr == '\0' && endptr != str);
}

/* Função chamada pela interrupção do botão, que altera o estado dos LEDS. */
void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	    if (led_mode == 0){
            gpio_pin_set_dt(&led, 1);
            gpio_pin_set_dt(&led1, 0);
        } else if (led_mode == 1){
            gpio_pin_set_dt(&led, 0);
            gpio_pin_set_dt(&led1, 1);
        } else if (led_mode == 2){
            gpio_pin_set_dt(&led, 1);
            gpio_pin_set_dt(&led1, 1);
        } else {
            gpio_pin_set_dt(&led, 1);
            gpio_pin_set_dt(&led1, 0);
            led_mode = 0;
            return;
        }

        led_mode++;
}


/* --- COMANDOS DO SHELL --- */

/* Comando para controlar a velocidade do LED*/
static int cmd_led_control(const struct shell *shell, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_print(shell, "Uso: led <speed_em_ms>");
        return -EINVAL;
    }

    if (is_string_number(argv[1]) != 0){

        if (strlen(argv[1]) > 10 || strlen(argv[1]) == 0){
            shell_print(shell, "Velocidade do LED inválida.");
            return -EINVAL;
        }

        if (argv[1][0] == '-') {
            shell_print(shell, "Velocidade do LED inválida.");
            return -EINVAL;
        }

        uint32_t user_input_led_speed = atoi(argv[1]);
        led_speed = user_input_led_speed;
        return 0;
    }
    shell_print(shell, "Velocidade do LED inválida.");
    return -EINVAL;
}

/* Comando para exibir informações da tarefa do LED */
static int cmd_task_info(const struct shell *shell, size_t argc, char **argv)
{
    char *led_details[] = {"ambos, alternando.", "apenas LED verde.", "apenas LED vermelho.", "ambos, sincronizados."};
    shell_print(shell, "--- Informacoes da Tarefa de Tempo Real (LED) ---");
    shell_print(shell, "Velocidade do LED: %i", led_speed);
    shell_print(shell, "LED ligado: %s", led_details[led_mode]);
    shell_print(shell, "Prioridade: %d", LED_PRIORITY);
    
    // Para informações mais detalhadas, o Zephyr já fornece um comando
    shell_print(shell, "Para detalhes de runtime (CPU, stack), use o comando: 'kernel threads'");

    return 0;
}

static void thread_info_callback(struct thread_analyzer_info *info)
{
    printk("Task: %s\n", info->name);
    printk(" Stack: %u/%u bytes (%.1f%% used)\n", 
           info->stack_used, info->stack_size, 
           (float)info->stack_used / info->stack_size * 100);
    printk("CPU usage: %u%%\n", info->utilization);
    printk("----------------\n");
}

static int cmd_task_kernel(const struct shell *shell, size_t argc, char **argv)
{
    shell_print(shell, "--- Informacoes das Tarefas instaladas ---");
    thread_analyzer_run(thread_info_callback, 0);
    // k_thread_foreach(thread_info_cb, (void *)shell);
    shell_print(shell, "--- Para informações específicas da tarefa, digite task_info <nome_tarefa> ---");
    return 0;
}

/* Registro dos comandos no sistema do Shell */
SHELL_CMD_REGISTER(led, NULL, "Controla a tarefa do LED (start/stop)", cmd_led_control);
SHELL_CMD_REGISTER(task_info, NULL, "Mostra informacoes da task informada.", cmd_task_info);
SHELL_CMD_REGISTER(system, NULL, "Mostra informações sobre todas as tarefas instaladas, heap livre e runtime das tarefas    ", cmd_task_kernel);

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

        /* Verifica se o dispositivo do LED1 está pronto */
    if (!device_is_ready(led1.port)) {
        LOG_ERR("Dispositivo do LED1 nao esta pronto");
        return -ENODEV;
    }

        /* Configura o pino do LED como saída */
    ret = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
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

    /* Configura o pino do botão */
    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n",
		       ret, button.port->name, button.pin);
		return 0;
	}

    /* Configura a interrupção do botão */
	ret = gpio_pin_interrupt_configure_dt(&button,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure interrupt on %s pin %d\n",
			ret, button.port->name, button.pin);
		return 0;
	}

    
	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);
	LOG_INF("Set up button at %s pin %d\n", button.port->name, button.pin);

    LOG_INF("Tarefa do LED criada com sucesso. Shell esta pronto.");

    return 0;
}


/*
1. Criar uma aplicação que tenha pelo menos uma tarefa de tempo real hard e e uma de tempo real soft.

Exemplos de tarefa de tempo real hard realtime:
- Calcular a FFT de um sinal de um canal do ADC (sinal gerado por gerador de sinal ou pelo DAC)
- implementar um filtro digital em um sinal capturado pelo ADC e jogá-lo filtrado no DAC
- controle de motor
- aquisição de dados de um microfone e processamento de audio

Exemplos de tarefa de tempo real soft realtime:
- escrita ou leitura de cartão SD
- envio de informações para uma nuvem
- a própria tarefa do shell
- atualização de uma tela gráfica
- leitura periódica de sensores (temperatura, acelerometro, etc)

2. Criar uma tarefa para o console/shell que permita o acesso às informações primárias
 do sistema (tarefas instaladas, heap livre e informações de runtime das tarefas).
Crie também um comando para acessar informações com relação às tarefas de tempo real.
Falta implementar: informações em relação às tarefas de tempo real.

Fazer uma melhoria no comando task_info e passar um parametro que será a tarefa que se deseja ver asinformações, atualmente só mostra do LED.
Fazer um comando help que mostra os comandos disponíveis. Fazer um print no shell quando inicia o sistema chamando essa função help.

3. Criar uma tarefa para piscar um led. done

4. Criar uma tarefa que executa alguma função pelo clique do botão da placa utilizada. done

*/