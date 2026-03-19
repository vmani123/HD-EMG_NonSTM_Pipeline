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

#include "driver/spi_slave.h"
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

#define SPI_MODE         0

#define NUM_CHANNELS     64
#define BYTES_PER_SAMPLE 2
#define MAGIC_LEN        2
#define SPI_BUF_SIZE     (MAGIC_LEN + NUM_CHANNELS * BYTES_PER_SAMPLE)  // 2 + 128 = 130 bytes

/* ======= Frame format ======= */
// Each 130-byte SPI transaction:
//   byte[0..1]   = 0x0F 0x0F magic marker
//   byte[2..129] = 64 channels * 2 bytes each (RMS values, big-endian uint16)
#define MAGIC_BYTE_0     0x0F
#define MAGIC_BYTE_1     0x0F


/* ======= TCP batching configuration ======= */
#define TCP_BATCH_FRAMES  256                 // 256 * 130 = 33280 bytes
#define TCP_BATCH_SIZE    (SPI_BUF_SIZE * TCP_BATCH_FRAMES)

/* ======= SPI async / DMA queueing configuration ======= */
#define SPI_INFLIGHT      16
static spi_slave_transaction_t s_trans[SPI_INFLIGHT];
static uint8_t *s_rxbuf[SPI_INFLIGHT];

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

/* Stats (written in SPI task, read in stats task) */
static volatile uint32_t correct   = 0;
static volatile uint32_t incorrect = 0;

int handshake_complete = 0;

/* ======= SPI init ======= */
static void spi_slave_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_BUF_SIZE,
    };

    spi_slave_interface_config_t slvcfg = {
        .mode        = SPI_MODE,
        .spics_io_num = PIN_NUM_CS,
        .queue_size  = SPI_INFLIGHT,
        .flags       = 0,
    };

    ESP_ERROR_CHECK(spi_slave_initialize(SPI_HOST_USE, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "SPI slave initialized: mode=%d", SPI_MODE);

    // Allocate receive buffers in DMA-capable memory
    for (int i = 0; i < SPI_INFLIGHT; i++) {
        s_rxbuf[i] = (uint8_t *)heap_caps_malloc(SPI_BUF_SIZE, MALLOC_CAP_DMA);
        assert(s_rxbuf[i] != NULL);

        memset(&s_trans[i], 0, sizeof(s_trans[i]));
        s_trans[i].tx_buffer = NULL;
        s_trans[i].rx_buffer = s_rxbuf[i];
        s_trans[i].length    = SPI_BUF_SIZE * 8;
        s_trans[i].user      = (void *)(intptr_t)i;
    }
}

/* ======= SPI handshake ======= */
/**
 * Wait until the STM drives a transaction whose byte[0] is 0x0F.
 * The ESP blocks here — no polling, no empty reads.
 * The STM owns the clock so this only returns when real data arrives.
 */
static bool stm_handshake(void)
{
    esp_rom_printf("Waiting for handshake from STM...\n");

    int8_t txbuf[2] = {0x0F, 0x0F};
    spi_slave_transaction_t t = {
        .tx_buffer = txbuf,
        .rx_buffer = s_rxbuf[0],
        .length = 16 
        // .length    = SPI_BUF_SIZE * 8,
    };

    while (1) {
        // Blocks until STM actually drives a transaction
        ESP_ERROR_CHECK(spi_slave_transmit(SPI_HOST_USE, &t, portMAX_DELAY));

        if (s_rxbuf[0][0] == MAGIC_BYTE_0 && s_rxbuf[0][1] == MAGIC_BYTE_1) {
            esp_rom_printf("SPI Handshake Complete from ESP end.\n");
            handshake_complete = 1;
            return true;
        }

        esp_rom_printf("Handshake: unexpected magic=0x%02X%02X, retrying...\n", s_rxbuf[0][0], s_rxbuf[0][1]);
    }

    esp_rom_printf("SPI Handshake Complete.\n");
    return true;
}

/* ======= SPI async helpers ======= */
// Queue all SPI_INFLIGHT transactions into the hardware pipeline
static void spi_prime_async_reads(void)
{
    for (int i = 0; i < SPI_INFLIGHT; i++) {
        esp_err_t err = spi_slave_queue_trans(SPI_HOST_USE, &s_trans[i], portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "queue_trans failed: %s", esp_err_to_name(err));
        }
    }
}

// Dequeue one completed transaction, then immediately re-queue it
static uint8_t *spi_get_and_requeue(void)
{
    spi_slave_transaction_t *r = NULL;
    esp_err_t err = spi_slave_get_trans_result(SPI_HOST_USE, &r, portMAX_DELAY);
    if (err != ESP_OK || r == NULL) {
        ESP_LOGE(TAG, "get_trans_result failed: %s", esp_err_to_name(err));
        return NULL;
    }

    uint8_t *buf = (uint8_t *)r->rx_buffer;

    err = spi_slave_queue_trans(SPI_HOST_USE, r, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "re-queue failed: %s", esp_err_to_name(err));
    }

    return buf;
}

/* ======= Stats printer task ======= */
/**
 * Runs on core 0 alongside TCP. Prints magic-byte accuracy to serial ~1Hz.
 */
static void stats_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t c  = correct;
        uint32_t ic = incorrect;
        uint32_t total = c + ic;

        if (total == 0) {
            if(!handshake_complete){
                esp_rom_printf("Handshake incomplete.");
            }            
            esp_rom_printf("[stats] No frames validated yet\n");
        } else {
            uint32_t acc_milli = (c * 1000) / total;
            esp_rom_printf("[stats] frames_checked=%lu  correct=%lu  incorrect=%lu  accuracy=%lu.%lu%%\n",
                           (unsigned long)total,
                           (unsigned long)c,
                           (unsigned long)ic,
                           (unsigned long)(acc_milli / 10),
                           (unsigned long)(acc_milli % 10));
        }
    }
}

/* ======= SPI Producer Task ======= */
static void spi_task(void *arg)
{
    (void)arg;

    // Handshake once — blocks until STM sends a valid magic-byte frame
    stm_handshake();
    xEventGroupSetBits(g_evt, HANDSHAKE_DONE_BIT);

    // Prime async pipeline once
    spi_prime_async_reads();

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
                xQueueSendToFront(free_q, &item, 0);
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }

            // Copy SPI frame into batch buffer
            memcpy(dst + filled, frame, SPI_BUF_SIZE);
            filled += SPI_BUF_SIZE;

            // Validate both magic bytes on every frame
            if (frame[0] == MAGIC_BYTE_0 && frame[1] == MAGIC_BYTE_1) {
                correct++;
            } else {
                incorrect++;
            }
        }

        if (filled == TCP_BATCH_SIZE) {
            item.len = TCP_BATCH_SIZE;

            if (xQueueSend(filled_q, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
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

        // Wait for SPI handshake to complete once
        xEventGroupWaitBits(g_evt, HANDSHAKE_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        // Mark connected so SPI task begins producing
        xEventGroupSetBits(g_evt, CONNECTED_BIT);

        while (1) {
            batch_item_t item;

            if (xQueueReceive(filled_q, &item, portMAX_DELAY) != pdTRUE) {
                continue;
            }

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

                xQueueSend(free_q, &item, 0);

                xEventGroupClearBits(g_evt, CONNECTED_BIT);
                shutdown(sock, 0);
                close(sock);

                while (xQueueReceive(filled_q, &item, 0) == pdTRUE) {
                    xQueueSend(free_q, &item, 0);
                }

                goto reconnect;
            }

            xQueueSend(free_q, &item, 0);
        }

reconnect:
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ======= Start function (call from app_main) ======= */
void tcp_client(void)
{
    // Init SPI slave
    spi_slave_init();

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

    // Core 0: TCP (shares core with Wi-Fi) + stats printer
    // Core 1: SPI (dedicated, time-sensitive)
    xTaskCreatePinnedToCore(tcp_task,   "tcp_task",   8192, NULL, 12, NULL, 0);
    xTaskCreatePinnedToCore(stats_task, "stats_task", 2048, NULL,  5, NULL, 0);
    xTaskCreatePinnedToCore(spi_task,   "spi_task",   8192, NULL, 13, NULL, 1);
}
