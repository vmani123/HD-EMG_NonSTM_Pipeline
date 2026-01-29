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
// static const char *payload = "Message from ESP32 ";

/* ======= SPI configuration (change pins/mode/clock to match your slave) ======= */
#define SPI_HOST_USE     VSPI_HOST   // VSPI (SPI3).
#define PIN_NUM_MISO     19          //Default, not GPIO matrix pins 
#define PIN_NUM_MOSI     23
#define PIN_NUM_SCLK     18
#define PIN_NUM_CS       5

#define SPI_CLOCK_HZ     (10*1000*1000)   //
#define SPI_MODE         0               // CPOL/CPHA = 0; set 0/1/2/3 per your slave

#define SPI_BUF_SIZE   64

static spi_device_handle_t spi = NULL;

char spiTransactionBuf[SPI_BUF_SIZE] = {0};


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
        .queue_size = 1,
    };
    
    ret = spi_bus_initialize(SPI_HOST_USE, &buscfg, 0);
    ESP_ERROR_CHECK(ret);
    ret = spi_bus_add_device(SPI_HOST_USE, &stm32cfg, &spi);
    

    int khz;
    spi_device_get_actual_freq(spi,&khz);
    printf("True SPI Frequency: %dkhz \n", khz);    

    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "SPI master initialized: mode=%d, %d Hz", SPI_MODE, SPI_CLOCK_HZ);
}



void spi_read_bytes()
{
    
    // printf("Trying to read attempt: %d \n", attempts);
    char buf[2] = {0};
    spi_transaction_t receive = {
                                .tx_buffer = NULL,
                                .rx_buffer = spiTransactionBuf,
                                .length = SPI_BUF_SIZE*8,
                                .rxlength = SPI_BUF_SIZE*8};

    if(spi_device_polling_transmit(spi, &receive)!=ESP_OK)
    {
        ESP_LOGE(TAG, "receive issue");
    }

    int matched = 1;
    int allZeros = 1;
    for(int i=0;i<64;i++)
    {
        if(spiTransactionBuf[i]!=i && spiTransactionBuf[i] != 0)
        {
            // printf("Expected: %d Received: %d \n", i, spiTransactionBuf[i]);
            matched =0;
        }

        if(spiTransactionBuf[i]!=0)
        {
            allZeros = 0;
        }
    }

    
    int total = correct + incorrect;
    if (total > 0) accuracy = (float)correct / (float)total;

    if(matched && !allZeros) {
        // printf("All bytes received. Total correct %d, Current accuracy: %f \n", correct,accuracy);
        correct++;
    }
    else{ 
        // printf("Mismatch. Current Accuracy: %f \n", accuracy);  
        incorrect++;
    }
}

bool stm_handshake()
{
    printf("Shaking Hands... \n");
    bool receivedProperSequence = false;
    
    //Wait for proper sequence to be recevied.
    int time = 0;
    while(!receivedProperSequence)
    {
        printf("Handshake failed at time t = %dms\n", time*500);
        spi_transaction_t receive = {
            .tx_buffer = NULL,
            .rx_buffer = spiTransactionBuf,
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
            if(spiTransactionBuf[i]!=i)
            {
                receivedProperSequence = false;
            }
        }        
        time++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("SPI Handshake Complete.");
    return true;
}

void tcp_client(void)
{
    char host_ip[] = HOST_IP_ADDR;
    int addr_family = 0;
    int ip_protocol = 0;

    
    // gpio_config_t io_conf = {};
    // io_conf.pin_bit_mask = ((1ULL << 5));
    // io_conf.mode = GPIO_MODE_OUTPUT;
    // io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    // gpio_config(&io_conf);      

    // Initialize SPI once up front
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


        long i = 0;
        while (1) {
            // 1) Read a block from SPI (blocking call)
            spi_read_bytes();
            i++;
           
            size_t sent_total = 0;
            while (sent_total < SPI_BUF_SIZE) {
                
                ssize_t n;
                n = send(sock, spiTransactionBuf + sent_total,
                                    SPI_BUF_SIZE - sent_total, 0);
                
                if (n > 0) {
                    sent_total += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EINTR)) 
                { 
                    ESP_LOGE(TAG, "TCP send failed: errno %d", errno);

                    break;
                }   

            }
                
            if((i%200)==0)
            {
                printf("Total correct %d, Current accuracy: %f \n", correct,accuracy);
            }            
            // vTaskDelay(pdMS_TO_TICKS(1));
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
