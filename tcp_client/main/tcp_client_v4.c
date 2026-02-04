/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "sdkconfig.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "esp_netif.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "stdbool.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#if defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
#include "addr_from_stdin.h"
#endif

#if defined(CONFIG_EXAMPLE_IPV4)
#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV4_ADDR
#elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
#define HOST_IP_ADDR ""
#endif

#define PORT CONFIG_EXAMPLE_PORT

static const char *TAG = "example";

/* ======= SPI configuration ======= */
#define SPI_HOST_USE     VSPI_HOST
#define PIN_NUM_MISO     19
#define PIN_NUM_MOSI     23
#define PIN_NUM_SCLK     18
#define PIN_NUM_CS       5

#define SPI_CLOCK_HZ     (10*1000*1000)
#define SPI_MODE         0

#define SPI_BUF_SIZE     64

/* ======= TCP batching configuration ======= */
#define TCP_BATCH_FRAMES  256                 // 256 * 64 = 16384 bytes
#define TCP_BATCH_SIZE    (SPI_BUF_SIZE * TCP_BATCH_FRAMES)

/* ======= SPI async / DMA queueing configuration ======= */
#define SPI_INFLIGHT      16
static spi_transaction_t s_trans[SPI_INFLIGHT];
static uint8_t *s_rxbuf[SPI_INFLIGHT];
static spi_device_handle_t spi = NULL;

/* ======= Double-buffering between tasks ======= */
#define NUM_BATCH_BUFS 2

typedef struct {
    uint8_t *buf;
    size_t   len;     // always TCP_BATCH_SIZE in this design
} batch_item_t;

static QueueHandle_t free_q   = NULL;
static QueueHandle_t filled_q = NULL;

static EventGroupHandle_t g_evt = NULL;
#define CONNECTED_BIT       (1 << 0)
#define HANDSHAKE_DONE_BIT  (1 << 1)

/* Stats (written in SPI task, read in TCP task) */
static volatile int correct = 0;
static volatile int incorrect = 0;

/* ======= SPI init ======= */
static void spi_master_init(void)
{
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512*8,   // bytes; OK for 64B frames
    };

    spi_device_interface_config_t stm32cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = SPI_MODE,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = SPI_INFLIGHT,
    };

    ret = spi_bus_initialize(SPI_HOST_USE, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    ret = spi_bus_add_device(SPI_HOST_USE, &stm32cfg, &spi);
    ESP_ERROR_CHECK(ret);

    int actual_hz_or_khz = 0;
    spi_device_get_actual_freq(spi, &actual_hz_or_khz);
    esp_rom_printf("SPI actual freq (raw): %d\n", actual_hz_or_khz);

    ESP_LOGI(TAG, "SPI master initialized: mode=%d, target=%d Hz", SPI_MODE, SPI_CLOCK_HZ);

    for (int i = 0; i < SPI_INFLIGHT; i++) {
        s_rxbuf[i] = (uint8_t *)heap_caps_malloc(SPI_BUF_SIZE, MALLOC_CAP_DMA);
        assert(s_rxbuf[i] != NULL);

        memset(&s_trans[i], 0, sizeof(s_trans[i]));
        s_trans[i].tx_buffer = NULL;
        s_trans[i].rx_buffer = s_rxbuf[i];
        s_trans[i].length    = SPI_BUF_SIZE * 8;
        s_trans[i].rxlength  = SPI_BUF_SIZE * 8;
        s_trans[i].user      = (void *)(intptr_t)i;
    }
}

/* ======= SPI handshake (kept synchronous) ======= */
static bool stm_handshake(void)
{
    esp_rom_printf("Shaking Hands...\n");
    bool receivedProperSequence = false;
    int tries = 0;

    while (!receivedProperSequence) {
        esp_rom_printf("Handshake attempt %d\n", tries);

        spi_transaction_t receive = {
            .tx_buffer = NULL,
            .rx_buffer = s_rxbuf[0],
            .length = SPI_BUF_SIZE * 8,
            .rxlength = SPI_BUF_SIZE * 8
        };

        if (spi_device_polling_transmit(spi, &receive) != ESP_OK) {
            ESP_LOGE(TAG, "Handshake receive issue");
        }

        receivedProperSequence = true;
        for (int i = 0; i < 64; i++) {
            if (s_rxbuf[0][i] != (uint8_t)i) {
                receivedProperSequence = false;
                break;
            }
        }

        tries++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_rom_printf("SPI Handshake Complete.\n");
    return true;
}

/* ======= SPI async helpers ======= */
static void spi_prime_async_reads(void)
{
    for (int i = 0; i < SPI_INFLIGHT; i++) {
        esp_err_t err = spi_device_queue_trans(spi, &s_trans[i], portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "queue_trans failed: %s", esp_err_to_name(err));
        }
    }
}

static uint8_t *spi_get_and_requeue(void)
{
    spi_transaction_t *r = NULL;
    esp_err_t err = spi_device_get_trans_result(spi, &r, portMAX_DELAY);
    if (err != ESP_OK || r == NULL) {
        ESP_LOGE(TAG, "get_trans_result failed: %s", esp_err_to_name(err));
        return NULL;
    }

    uint8_t *buf = (uint8_t *)r->rx_buffer;

    err = spi_device_queue_trans(spi, r, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "re-queue failed: %s", esp_err_to_name(err));
    }

    return buf;
}

/* ======= SPI Producer Task ======= */
static void spi_task(void *arg)
{
    (void)arg;

    // Acquire SPI bus once and keep it
    spi_device_acquire_bus(spi, portMAX_DELAY);

    // Handshake once (as in your original flow)
    stm_handshake();
    xEventGroupSetBits(g_evt, HANDSHAKE_DONE_BIT);

    // Prime async pipeline once
    spi_prime_async_reads();

    uint32_t validate_count = 0;
    uint32_t validate_mod = 100; // validate every 100 frames

    while (1) {
        // Wait until TCP is connected before producing (prevents backlog growth)
        xEventGroupWaitBits(g_evt, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        // Get an empty batch buffer to fill
        batch_item_t item;
        if (xQueueReceive(free_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Fill one full batch
        uint8_t *dst = item.buf;
        size_t filled = 0;

        while (filled < TCP_BATCH_SIZE) {
            uint8_t *frame = spi_get_and_requeue();
            if (!frame) {
                // Return buffer and retry later
                xQueueSendToFront(free_q, &item, 0);
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }

            // Copy SPI frame into batch buffer
            memcpy(dst + filled, frame, SPI_BUF_SIZE);
            filled += SPI_BUF_SIZE;

            // Validate every 100 frames (low overhead)
            validate_count++;
            if (validate_count >= validate_mod) {
                validate_count = 0;

                int matched = 1;
                int allZeros = 1;
                for (int k = 0; k < 64; k++) {
                    if (frame[k] != (uint8_t)k && frame[k] != 0) matched = 0;
                    if (frame[k] != 0) allZeros = 0;
                }
                if (matched && !allZeros) correct++;
                else incorrect++;
            }
        }

        if (filled == TCP_BATCH_SIZE) {
            item.len = TCP_BATCH_SIZE;

            // Publish filled buffer to TCP task
            // If TCP disconnects, this send could block; keep it bounded.
            if (xQueueSend(filled_q, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
                // If we couldn't publish, return buffer to free list
                xQueueSend(free_q, &item, 0);
            }
        }
    }
}

/* ======= TCP Consumer Task ======= */
static void tcp_task(void *arg)
{
    (void)arg;

    char host_ip[] = HOST_IP_ADDR;
    int64_t start_us = 0;
    int64_t last_print_us = 0;

    while (1) {
        int addr_family = 0;
        int ip_protocol = 0;

#if defined(CONFIG_EXAMPLE_IPV4)
        struct sockaddr_in dest_addr;
        inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
#elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
        struct sockaddr_storage dest_addr = { 0 };
        ESP_ERROR_CHECK(get_addr_from_stdin(PORT, SOCK_STREAM, &ip_protocol, &addr_family, &dest_addr));
#endif

        int sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, PORT);

        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        ESP_LOGI(TAG, "Successfully connected to PC");

        // Wait for SPI handshake to be completed once
        xEventGroupWaitBits(g_evt, HANDSHAKE_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        // Mark connected so SPI task begins producing
        xEventGroupSetBits(g_evt, CONNECTED_BIT);

        start_us = esp_timer_get_time();
        last_print_us = 0;

        while (1) {
            batch_item_t item;

            // Wait for next filled buffer (blocks)
            if (xQueueReceive(filled_q, &item, portMAX_DELAY) != pdTRUE) {
                continue;
            }

            // Send entire buffer
            size_t sent_total = 0;
            while (sent_total < item.len) {
                ssize_t n = send(sock, item.buf + sent_total, item.len - sent_total, 0);
                if (n > 0) {
                    sent_total += (size_t)n;
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }

                ESP_LOGE(TAG, "TCP send failed: errno %d", errno);

                // Return buffer before reconnecting
                xQueueSend(free_q, &item, 0);

                // Disconnect handling
                xEventGroupClearBits(g_evt, CONNECTED_BIT);
                shutdown(sock, 0);
                close(sock);

                // Drain any already-filled buffers back to free list (avoid deadlock)
                while (xQueueReceive(filled_q, &item, 0) == pdTRUE) {
                    xQueueSend(free_q, &item, 0);
                }

                goto reconnect;
            }

            // Return buffer to free list
            xQueueSend(free_q, &item, 0);

            // 1 Hz stats print (very low overhead)
            int64_t now_us = esp_timer_get_time() - start_us;
            if (now_us - last_print_us >= 1000000) {
                last_print_us = now_us;

                int c = (int)correct;
                int ic = (int)incorrect;
                int total = c + ic;
                int acc_milli = (total > 0) ? (c * 1000) / total : 0;

                // total samples checked = total validations * 100 (since validate every 100 frames)
                esp_rom_printf("t=%lld ms  validated_samples=%d  acc=%d.%03d\n",
                               (long long)(now_us / 1000),
                               total * 100,
                               acc_milli / 1000, acc_milli % 1000);
            }
        }

reconnect:
        // loop reconnect
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ======= Start function (call from app_main) ======= */
void tcp_client(void)
{
    // Init SPI once
    spi_master_init();

    // Create sync primitives
    g_evt = xEventGroupCreate();
    assert(g_evt);

    free_q   = xQueueCreate(NUM_BATCH_BUFS, sizeof(batch_item_t));
    filled_q = xQueueCreate(NUM_BATCH_BUFS, sizeof(batch_item_t));
    assert(free_q && filled_q);

    // Allocate two batch buffers (internal RAM is fastest for memcpy + TCP)
    for (int i = 0; i < NUM_BATCH_BUFS; i++) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(TCP_BATCH_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        assert(buf);
        batch_item_t item = { .buf = buf, .len = TCP_BATCH_SIZE };
        xQueueSend(free_q, &item, portMAX_DELAY);
    }

    // Create tasks pinned to different cores
    // ESP32: Core 0 often busier with Wi-Fi; common pattern is TCP on core 0, SPI on core 1.
    xTaskCreatePinnedToCore(tcp_task, "tcp_task", 8192, NULL, 12, NULL, 0);
    xTaskCreatePinnedToCore(spi_task, "spi_task", 8192, NULL, 13, NULL, 1);
}
