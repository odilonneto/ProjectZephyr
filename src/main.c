#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h> // api do shel, comunicação
#include <stdlib.h>             // para atoi
#include <string.h>             // para strlen, strtol
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/devicetree.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* --- CONFIGURAÇÃO DOS LEDS, BOTÕES E THREADS --- */

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define SW0_NODE DT_ALIAS(sw0)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)
#define DAC_NODE DT_PHANDLE(ZEPHYR_USER_NODE, dac)
#define DAC_CHANNEL_ID DT_PROP(ZEPHYR_USER_NODE, dac_channel_id)
#define DAC_RESOLUTION DT_PROP(ZEPHYR_USER_NODE, dac_resolution)
#define DT_SPEC_AND_COMMA(node_id, prop, idx) \
    ADC_DT_SPEC_GET_BY_IDX(node_id, idx),
static const struct device *const dac_dev = DEVICE_DT_GET(DAC_NODE);

static const struct dac_channel_cfg dac_ch_cfg = {
    .channel_id = DAC_CHANNEL_ID,
    .resolution = DAC_RESOLUTION,
#if defined(CONFIG_DAC_BUFFER_NOT_SUPPORT)
    .buffered = false,
#else
    .buffered = true,
#endif /* CONFIG_DAC_BUFFER_NOT_SUPPORT */
};

static const struct adc_dt_spec adc_channels[] = {
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels,
                         DT_SPEC_AND_COMMA)};

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios,
                                                              {0});
uint64_t sample_speed = 1000;
static struct gpio_callback button_cb_data;
volatile uint32_t led_speed = 1000;
volatile uint8_t led_mode = 0; // 0 = leds alternando, 1 = apenas led verde, 2 = apenas led vermelho, 3 = leds sincronizados

// Variáveis para monitoramento ADC/DAC
volatile uint32_t last_adc_value = 0;
volatile uint32_t last_dac_value = 0;
volatile uint32_t last_adc_mv = 0;
volatile uint8_t adc_dac_enable_print = 0;

/* Estrutura para passar informações para o callback */
struct task_search_info {
    const struct shell *shell;
    const char *task_name;
    bool found;
};

struct task_search_info search_info_g;

#define LED_STACK_SIZE 512
#define LED_PRIORITY 5 // Prioridade da tarefa do LED
#define FILTER_PRIORITY 3
#define FILTER_STACK_SIZE 1024
const uint16_t dac_values = 1U << DAC_RESOLUTION;

const uint16_t sleep_time = 4096 / dac_values > 0 ? 4096 / dac_values : 1;
K_THREAD_STACK_DEFINE(led_stack, LED_STACK_SIZE);
K_THREAD_STACK_DEFINE(filter_stack, FILTER_STACK_SIZE);
struct k_thread led_thread_data;
struct k_thread dac_thread_data;
k_tid_t led_thread_id;
k_tid_t filter_thread_id;

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

    while (1)
    {

        if (led_mode == 0 || led_mode == 3)
        {
            gpio_pin_toggle_dt(&led);
            gpio_pin_toggle_dt(&led1);
        }
        else if (led_mode == 1)
        {
            gpio_pin_toggle_dt(&led);
        }
        else if (led_mode == 2)
        {
            gpio_pin_toggle_dt(&led1);
        }

        k_sleep(K_MSEC(led_speed));
        
    }
}

void filter_task()
{

    uint8_t err;
    uint16_t buf;
    struct adc_sequence sequence = {
        .buffer = &buf,
        /* buffer size in bytes, not number of samples */
        .buffer_size = sizeof(buf),
    };

    /* Configure channels individually prior to sampling. */
    for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++)
    {
        if (!adc_is_ready_dt(&adc_channels[i]))
        {
            printk("ADC controller device %s not ready\n", adc_channels[i].dev->name);
            return;
        }

        err = adc_channel_setup_dt(&adc_channels[i]);
        if (err < 0)
        {
            printk("Could not setup channel #%d (%d)\n", i, err);
            return;
        }
    }

    uint8_t sample_size = 30;
    uint16_t samples_to_filter[sample_size];
    uint8_t current_index = 0;
    for (uint8_t i = 0; i < sample_size; i++)
    {
        samples_to_filter[i] = 0.0;
    }

    while (1)
    {

        for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++)
        {
            int32_t val_mv;

            (void)adc_sequence_init_dt(&adc_channels[i], &sequence);

            err = adc_read_dt(&adc_channels[i], &sequence);
            if (err < 0)
            {
                printk("Could not read (%d)\n", err);
                continue;
            }

            if (adc_channels[i].channel_cfg.differential)
            {
                val_mv = (int32_t)((int16_t)buf);
            }
            else
            {
                val_mv = (int32_t)buf;
            }

            samples_to_filter[current_index] = val_mv;
            current_index++;

            if (current_index >= sample_size)
            {
                current_index = 0;
            }

            float sum = 0.0;
            for (uint8_t i = 0; i < sample_size; i++)
            {
                sum += samples_to_filter[i];
            }

            uint32_t filtered_value = (uint32_t)(sum / sample_size);

            // Atualiza variáveis para monitoramento
            last_adc_value = val_mv;
            last_dac_value = filtered_value;

            dac_write_value(dac_dev, DAC_CHANNEL_ID, filtered_value);

            err = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
            if (err >= 0)
            {
                last_adc_mv = val_mv;
            }
        }

            k_sleep(K_USEC(sample_speed));
    }
}

/* Função axuiliar que verifica se uma string pode ser convertida a um número
    Retorna 1 se sim, 0 se não.
*/
uint8_t is_string_number(const char *str)
{
    char *endptr;
    long val = strtol(str, &endptr, 10);
    (void)val;  // Corrigido: casting correto
    return (*endptr == '\0' && endptr != str);
}

/* Função chamada pela interrupção do botão, que altera o estado dos LEDS. */
void button_pressed(const struct device *dev, struct gpio_callback *cb,
                    uint32_t pins)
{
    if (led_mode == 0)
    {
        gpio_pin_set_dt(&led, 1);
        gpio_pin_set_dt(&led1, 0);
    }
    else if (led_mode == 1)
    {
        gpio_pin_set_dt(&led, 0);
        gpio_pin_set_dt(&led1, 1);
    }
    else if (led_mode == 2)
    {
        gpio_pin_set_dt(&led, 1);
        gpio_pin_set_dt(&led1, 1);
    }
    else
    {
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
    if (argc != 2)
    {
        shell_print(shell, "Uso: led <speed_em_ms>");
        return -EINVAL;
    }

    if (is_string_number(argv[1]) != 0)
    {

        if (strlen(argv[1]) > 10 || strlen(argv[1]) == 0)
        {
            shell_print(shell, "Velocidade do LED inválida.");
            return -EINVAL;
        }

        if (argv[1][0] == '-')
        {
            shell_print(shell, "Velocidade do LED inválida.");
            return -EINVAL;
        }

        uint32_t user_input_led_speed = atoi(argv[1]);
        led_speed = user_input_led_speed;
        shell_print(shell, "Frequência de amostragem alterada para: %d hz", led_speed);
        return 0;
    }
    shell_print(shell, "Velocidade do LED inválida.");
    return -EINVAL;
}

/* Função para mostrar ajuda na inicialização */
void show_startup_help(void)
{
    printk("\n");
    printk("=== Sistema Zephyr Tempo Real Iniciado ===\n");
    printk("Digite 'help' para ver comandos disponíveis\n");
    printk("==========================================\n");
}

/* Callback para buscar informações de uma tarefa específica */
static void specific_task_info_callback(const struct k_thread *thread, void *user_data)
{
    // Cast para remover const - necessário para compatibilidade da API
    k_tid_t tid = (k_tid_t)thread;
    const char *name = k_thread_name_get(tid);
    
    if (search_info_g.task_name == NULL || strcmp(name, search_info_g.task_name) == 0)
    {
        search_info_g.found = true;
        struct k_thread_runtime_stats stats;
        k_thread_runtime_stats_get(tid, &stats);

        /* Buffer para armazenar a string do estado da thread */
        char state_buf[32];

        shell_print(search_info_g.shell, "=== Informações da Tarefa: %s ===", name);
        
        shell_print(search_info_g.shell, "Uso de CPU: %llu ciclos", stats.execution_cycles);

        shell_print(search_info_g.shell, "Estado: %s", k_thread_state_str(tid, state_buf, sizeof(state_buf)));
        
        // Informações específicas para tarefas de tempo real
        if (strcmp(name, "led_task") == 0)
        {
            char *led_details[] = {"ambos, alternando", "apenas LED verde",
                                   "apenas LED vermelho", "ambos, sincronizados"};
            shell_print(search_info_g.shell, "Tipo: Tempo Real Soft");
            shell_print(search_info_g.shell, "Prioridade: %d", LED_PRIORITY);
            shell_print(search_info_g.shell, "Velocidade: %d ms", led_speed);
            shell_print(search_info_g.shell, "Modo LED: %s", led_details[led_mode]);
        }
        else if (strcmp(name, "filter_task") == 0)
        {
            shell_print(search_info_g.shell, "Tipo: Tempo Real Hard");
            shell_print(search_info_g.shell, "Prioridade: %d", FILTER_PRIORITY);
            shell_print(search_info_g.shell, "Função: Filtro digital ADC->DAC");
            shell_print(search_info_g.shell, "Último ADC: %d (%d mV)",
                        last_adc_value, last_adc_mv);
            shell_print(search_info_g.shell, "Último DAC: %d", last_dac_value);
            shell_print(search_info_g.shell, "Frequência de amostragem: %i", 1000000/sample_speed);
        }
        shell_print(search_info_g.shell, "=====================================");
    }
}

/* Comando para exibir informações da tarefa do LED */
static int cmd_task_info(const struct shell *shell, size_t argc, char **argv)
{
    search_info_g.shell = shell;
    search_info_g.task_name = NULL;
    search_info_g.found = false;

    if (argc == 2)
    {
        search_info_g.task_name = argv[1];
        shell_print(shell, "Buscando informações da tarefa: %s", argv[1]);
    }
    else if (argc == 1)
    {
        shell_print(shell, "Mostrando informações de todas as tarefas:");
    }
    else
    {
        shell_print(shell, "Uso: task_info [nome_da_tarefa]");
        shell_print(shell, "Tarefas disponíveis: led_task, filter_task");
        return -EINVAL;
    }

    k_thread_foreach(specific_task_info_callback, NULL);

    if (argc == 2 && !search_info_g.found)
    {
        shell_print(shell, "Tarefa '%s' não encontrada.", argv[1]);
        shell_print(shell, "Tarefas disponíveis: led_task, filter_task");
        return -ENOENT;
    }

    return 0;
}

/* Callback para listar todas as tarefas */
static void thread_list_callback(const struct k_thread *thread, void *user_data)
{
    // Cast para remover const - necessário para compatibilidade da API
    k_tid_t tid = (k_tid_t)thread;
    const char *name = k_thread_name_get(tid);
    
    /* Buffer para armazenar a string do estado da thread */
    char state_buf[32];
    
    shell_print(search_info_g.shell, "%-15s | %s",
                name,
                /* Chamada correta, passando o buffer */
                k_thread_state_str(tid, state_buf, sizeof(state_buf)));
}

static int cmd_system_info(const struct shell *shell, size_t argc, char **argv)
{
    search_info_g.shell = shell;
    search_info_g.task_name = NULL;
    search_info_g.found = false;

    shell_print(shell, "=== Informações do Sistema ===");
    shell_print(shell, "Tarefas Instaladas:");
    shell_print(shell, "Nome            | Estado");
    shell_print(shell, "----------------|-------");

    k_thread_foreach(thread_list_callback, NULL);

    shell_print(shell, "");

    shell_print(shell, "Heap configurado: %d bytes", CONFIG_HEAP_MEM_POOL_SIZE);

    shell_print(shell, "");
    shell_print(shell, "Para informações detalhadas de uma tarefa específica:");
    shell_print(shell, "  task_info <nome_tarefa>");

    return 0;
}

/* Comando para controlar saída ADC/DAC */
static int cmd_adc_dac_control(const struct shell *shell, size_t argc, char **argv)
{
     if (argc != 2)
    {
        shell_print(shell, "Uso: adc_dac <frequencia_de_amostragem_hz>");
        return -EINVAL;
    }

    if (is_string_number(argv[1]) != 0)
    {

        if (strlen(argv[1]) > 10 || strlen(argv[1]) == 0)
        {
            shell_print(shell, "Frequencia inválida.");
            return -EINVAL;
        }

        if (argv[1][0] == '-')
        {
            shell_print(shell, "Frequência inválida.");
            return -EINVAL;
        }
        if (atoi(argv[1]) < 1 || atoi(argv[1]) > 100000){
            shell_print(shell, "Frequência de amostragem inválida. Digite um valor entre 1 e 100khz");
        }
        uint32_t user_input_sample_frequency = atoi(argv[1]);
        sample_speed = ( 1 / user_input_sample_frequency ) * 1000000; // Período em microssegundos
        shell_print(shell, "Frequencia de amostragem alterada para: %d Hz ", user_input_sample_frequency);
        return 0;
    }
    shell_print(shell, "Frequência inválida.");
    return -EINVAL;
}

/* Comando para mostrar informações do sistema */
static int cmd_help(const struct shell *shell, size_t argc, char **argv)
{
    shell_print(shell, "=== Comandos Disponíveis ===");
    shell_print(shell, "");
    shell_print(shell, "led <speed_ms>      - Controla velocidade do LED em ms");
    shell_print(shell, "task_info [nome]    - Informações detalhadas de tarefa");
    shell_print(shell, "                      Tarefas: led_task, filter_task");
    shell_print(shell, "system              - Informações do sistema completo");
    shell_print(shell, "adc_dac <frequencia_amostragem>    - Habilita/desabilita saída ADC/DAC");
    shell_print(shell, "help                - Mostra esta ajuda");
    shell_print(shell, "");
    shell_print(shell, "=== Informações das Tarefas de Tempo Real ===");
    shell_print(shell, "led_task     - Tempo Real Soft  (Prioridade %d)", LED_PRIORITY);
    shell_print(shell, "filter_task  - Tempo Real Hard  (Prioridade %d)", FILTER_PRIORITY);
    shell_print(shell, "");
    shell_print(shell, "Pressione o botão da placa para alterar modo dos LEDs");
    shell_print(shell, "============================");

    return 0;
}

/* Registro dos comandos no sistema do Shell */
SHELL_CMD_REGISTER(led, NULL, "Controla a velocidade do LED", cmd_led_control);
SHELL_CMD_REGISTER(task_info, NULL, "Informações detalhadas de tarefa", cmd_task_info);
SHELL_CMD_REGISTER(system, NULL, "Informações completas do sistema", cmd_system_info);
SHELL_CMD_REGISTER(adc_dac, NULL, "Controla saída ADC/DAC", cmd_adc_dac_control);
SHELL_CMD_REGISTER(help, NULL, "Mostra comandos disponíveis", cmd_help);

/* --- FUNÇÃO PRINCIPAL --- */
int main(void)
{
    int ret;

    LOG_INF("Aplicacao Zephyr com Shell Iniciada...");

    /* Verifica se o dispositivo do LED está pronto */
    if (!device_is_ready(led.port))
    {
        LOG_ERR("Dispositivo do LED nao esta pronto");
        return -ENODEV;
    }

    /* Configura o pino do LED como saída */
    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        LOG_ERR("Falha ao configurar o pino do LED: %d", ret);
        return ret;
    }

    /* Verifica se o dispositivo do LED1 está pronto */
    if (!device_is_ready(led1.port))
    {
        LOG_ERR("Dispositivo do LED1 nao esta pronto");
        return -ENODEV;
    }

    /* Configura o pino do LED como saída */
    ret = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
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
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure %s pin %d\n",
                ret, button.port->name, button.pin);
        return 0;
    }

    /* Configura a interrupção do botão */
    ret = gpio_pin_interrupt_configure_dt(&button,
                                          GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure interrupt on %s pin %d\n",
                ret, button.port->name, button.pin);
        return 0;
    }

    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    LOG_INF("Set up button at %s pin %d\n", button.port->name, button.pin);

    if (!device_is_ready(dac_dev))
    {
        printk("DAC device %s is not ready\n", dac_dev->name);
        return 0;
    }

    ret = dac_channel_setup(dac_dev, &dac_ch_cfg);

    if (ret != 0)
    {
        printk("Setting up of DAC channel failed with code %d\n", ret);
        return 0;
    }

    filter_thread_id = k_thread_create(&dac_thread_data, filter_stack,
                                       K_THREAD_STACK_SIZEOF(filter_stack),
                                       filter_task, NULL, NULL, NULL,
                                       FILTER_PRIORITY, 0, K_NO_WAIT);

    k_thread_name_set(filter_thread_id, "filter_task");

    LOG_INF("Tarefa do LED criada com sucesso. Shell esta pronto.");

    /* Aguarda um pouco para garantir que o shell esteja pronto */
    k_sleep(K_MSEC(2000));

    /* Mostra ajuda na inicialização */
    show_startup_help();

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

3. Criar uma tarefa para piscar um led. done

4. Criar uma tarefa que executa alguma função pelo clique do botão da placa utilizada. done

falta fazer: comando "adc_dac on" plota muito mas eh possivel fazer ele parar com "adc_dac off"
*/