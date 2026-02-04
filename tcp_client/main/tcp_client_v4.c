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
#include <netdb.h>            // struct addrinfo
#include <arpa/inet.h>
#include "esp_netif.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "stdbool.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"    // heap_caps_malloc for DMA-capable buffers
#include "esp_timer.h"        // esp_timer_get_time()
#include "esp_rom_sys.h"      // esp_rom_printf()

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

/* ======= SPI configuration (change pins/mode/clock to match your slave) ======= */
#define SPI_HOST_USE     VSPI_HOST   // VSPI (SPI3).
#define PIN_NUM_MISO     19          //Default, not GPIO matrix pins
#define PIN_NUM_MOSI     23
#define PIN_NUM_SCLK     18
#define PIN_NUM_CS       5

#define SPI_CLOCK_HZ     (10*1000*1000)
#define SPI_MODE         0

#define SPI_BUF_SIZE     64

/* ======= TCP batching configuration ======= */
#define TCP_BATCH_FRAMES  256 //16384 data
#define TCP_BATCH_SIZE    (SPI_BUF_SIZE * TCP_BATCH_FRAMES)

/* ======= SPI async / DMA queueing configuration ======= */
#define SPI_INFLIGHT      16   // number of queued DMA transactions (tune 4/8/16)
static spi_transaction_t s_trans[SPI_INFLIGHT];
static uint8_t *s_rxbuf[SPI_INFLIGHT];

static spi_device_handle_t spi = NULL;

/* Batch buffer for TCP sends */
static uint8_t tcpBatchBuf[TCP_BATCH_SIZE];
static size_t tcpBatchFill = 0;

#define SPI_TAG "spi_protocol"

int correct = 0;
int incorrect = 0;
float accuracy = 0.0f;

static void spi_master_init(void)
{
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512*8,
    };
    spi_device_interface_config_t stm32cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = SPI_MODE,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = SPI_INFLIGHT,     // allow multiple queued transactions
    };

    // Enable DMA: use SPI_DMA_CH_AUTO instead of 0
    ret = spi_bus_initialize(SPI_HOST_USE, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    ret = spi_bus_add_device(SPI_HOST_USE, &stm32cfg, &spi);
    ESP_ERROR_CHECK(ret);

    int khz;
    spi_device_get_actual_freq(spi,&khz);
    printf("True SPI Frequency: %dkhz \n", khz);

    ESP_LOGI(TAG, "SPI master initialized: mode=%d, %d Hz", SPI_MODE, SPI_CLOCK_HZ);

    // Allocate DMA-capable RX buffers and initialize reusable transactions
    for (int i = 0; i < SPI_INFLIGHT; i++) {
        s_rxbuf[i] = (uint8_t *)heap_caps_malloc(SPI_BUF_SIZE, MALLOC_CAP_DMA);
        assert(s_rxbuf[i] != NULL);

        memset(&s_trans[i], 0, sizeof(s_trans[i]));
        s_trans[i].tx_buffer = NULL;
        s_trans[i].rx_buffer = s_rxbuf[i];
        s_trans[i].length    = SPI_BUF_SIZE * 8;  // bits
        s_trans[i].rxlength  = SPI_BUF_SIZE * 8;  // bits
        s_trans[i].user      = (void *)(intptr_t)i;
    }
}

static bool stm_handshake(void)
{
    printf("Shaking Hands... \n");
    bool receivedProperSequence = false;

    int time = 0;
    while(!receivedProperSequence)
    {
        printf("Handshake failed at time t = %dms\n", time*500);

        spi_transaction_t receive = {
            .tx_buffer = NULL,
            .rx_buffer = s_rxbuf[0],
            .length = SPI_BUF_SIZE*8,
            .rxlength = SPI_BUF_SIZE*8
        };

        if(spi_device_polling_transmit(spi, &receive)!=ESP_OK)
        {
            ESP_LOGE(TAG, "receive issue");
        }

        receivedProperSequence = true;
        for(int i=0;i<64;i++)
        {
            if(s_rxbuf[0][i] != (uint8_t)i)
            {
                receivedProperSequence = false;
                break;
            }
        }

        time++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("SPI Handshake Complete.");
    return true;
}

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

void tcp_client(void)
{
    char host_ip[] = HOST_IP_ADDR;
    int addr_family = 0;
    int ip_protocol = 0;

    spi_master_init();
    spi_device_acquire_bus(spi, portMAX_DELAY);

    while (1) {
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

        int sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, PORT);

        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Successfully connected to PC");

        ESP_LOGI(TAG, "Attemping SPI handshake");
        stm_handshake();

        // Start queued async DMA reads after handshake
        spi_prime_async_reads();

        tcpBatchFill = 0;

        // Minimal-overhead print state (prints at most once per second)
        static int64_t last_print_us = 0;

        while (1) {
            uint8_t *frame = spi_get_and_requeue();
            if (!frame) {
                ESP_LOGE(TAG, "SPI async receive failed");
                goto tcp_reconnect;
            }

            // correctness/accuracy logic
            int matched = 1;
            int allZeros = 1;
            for (int k = 0; k < 64; k++) {
                if (frame[k] != (uint8_t)k && frame[k] != 0) matched = 0;
                if (frame[k] != 0) allZeros = 0;
            }

            if (matched && !allZeros) correct++;
            else incorrect++;

            // Append to TCP batch buffer
            if (tcpBatchFill + SPI_BUF_SIZE <= TCP_BATCH_SIZE) {
                memcpy(&tcpBatchBuf[tcpBatchFill], frame, SPI_BUF_SIZE);
                tcpBatchFill += SPI_BUF_SIZE;
            }

            // If batch full, send it
            if (tcpBatchFill == TCP_BATCH_SIZE) {
                size_t sent_total = 0;
                while (sent_total < tcpBatchFill) {
                    ssize_t n = send(sock,
                                     tcpBatchBuf + sent_total,
                                     tcpBatchFill - sent_total,
                                     0);

                    if (n > 0) {
                        sent_total += (size_t)n;
                        continue;
                    }
                    if (n < 0 && (errno == EINTR)) {
                        continue;
                    }

                    ESP_LOGE(TAG, "TCP send failed: errno %d", errno);
                    goto tcp_reconnect;
                }
                tcpBatchFill = 0;
            }

            // ---- Minimal overhead stats print: once per second, integer-only ----
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_print_us >= 1000000) { // 1 second
                last_print_us = now_us;

                int total = correct + incorrect;
                // accuracy in thousandths (avoid float formatting)
                int acc_milli = (total > 0) ? (correct * 1000) / total : 0;

                esp_rom_printf("t=%lld ms  correct=%d  incorrect=%d  acc=%d.%03d\n",
                               (long long)(now_us / 1000),
                               correct,
                               incorrect,
                               acc_milli / 1000, acc_milli % 1000);
            }
        }

tcp_reconnect:
        // attempt to flush any partially-filled batch before closing (best-effort)
        if (tcpBatchFill > 0) {
            size_t sent_total = 0;
            while (sent_total < tcpBatchFill) {
                ssize_t n = send(sock,
                                 tcpBatchBuf + sent_total,
                                 tcpBatchFill - sent_total,
                                 0);

                if (n > 0) {
                    sent_total += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EINTR)) {
                    continue;
                }
                break;
            }
            tcpBatchFill = 0;
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            spi_device_release_bus(spi);
            close(sock);
        }
    }
    spi_device_release_bus(spi);
}
