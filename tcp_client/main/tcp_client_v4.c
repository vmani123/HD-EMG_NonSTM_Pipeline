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
#define SPI_HOST_USE     VSPI_HOST   // VSPI (SPI3). Use HSPI_HOST if you prefer SPI2.
#define PIN_NUM_MISO     19
#define PIN_NUM_MOSI     23
#define PIN_NUM_SCLK     18
#define PIN_NUM_CS       5

#define SPI_CLOCK_HZ     (1000*1000)  // 1 MHz
#define SPI_MODE         0               // CPOL/CPHA = 0; set 0/1/2/3 per your slave

static spi_device_handle_t spi = NULL;
// uint8_t spi_buf[SPI_CHUNK_BYTES];


#define SPI_TAG "spi_protocol"

int attempts =0;
static void spi_master_init(void)
{
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512*8,  // ignored w/o DMA; leaving 0 is fine
    };    
    spi_device_interface_config_t stm32cfg = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = SPI_MODE,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
        .address_bits = 0 
    };
    
    ret = spi_bus_initialize(SPI_HOST_USE, &buscfg, 0);
    ESP_ERROR_CHECK(ret);
    ret = spi_bus_add_device(SPI_HOST_USE, &stm32cfg, &spi);
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "SPI master initialized: mode=%d, %d Hz", SPI_MODE, SPI_CLOCK_HZ);
}



void spi_read_bytes()
{
    printf("Trying to read attempt: %d \n", attempts);
    attempts++;
    char buf[2] = {0};
    spi_transaction_t receive = {.flags = SPI_TRANS_USE_RXDATA,
                                .tx_buffer = buf,
                                .length = 16,
                                .rxlength = 16};

    // gpio_set_level(PIN_NUM_CS, 0);
    int ret = spi_device_polling_transmit(spi, &receive);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI read operation failed\n");
    }
    printf("Data read: %d %d \n", receive.rx_data[0], receive.rx_data[1]);
    // vTaskDelay(1 / portTICK_PERIOD_MS);
    // gpio_set_level(PIN_NUM_CS, 1);  
    vTaskDelay(pdMS_TO_TICKS(500)); 
    // vTaskDelay(1000 / portTICK_PERIOD_MS);                              
}

// //fully taken from https://krutarthpurohit.medium.com/implementing-spi-protocol-on-esp32-idf-5-1-version-6f2383af1c22
// void spi_write_data()     // Function to write data at given address
// {
//     /*If MSB of addr is set the host will read data from slave.
//      *If MSB of addr is clear the host will write data on slave.
//      */
//     char* buf = "SPI Hello From ESP32 write data ";    
//     spi_transaction_t trans_desc = {
//         // Configure the transaction_structure
//         .tx_data = buf,                                        
//         .length = strlen(buf)*8,                                                  
//     };
//     gpio_set_level(PIN_NUM_CS, 0);                                      
//     printf("Writing '%s' data at %x\n", buf);
//     ret = spi_device_polling_transmit(spi, &trans_desc);                // spi_device_polling_transmit starts to transmit entire 'trans_desc' structure.
//     if (ret != ESP_OK)
//     {
//         ESP_LOGE(SPI_TAG, "SPI write operation failed\n");
//     }   
//     vTaskDelay(1 / portTICK_PERIOD_MS);                                 // Once data is transferred, we provide the delay and then higher the CS'
//     gpio_set_level(PIN_NUM_CS, 1);                                      // After CS' is high, the slave sill get unselected
//     vTaskDelay(1000 / portTICK_PERIOD_MS);   
// }


void tcp_client(void)
{
    // char rx_buffer[128];
    char host_ip[] = HOST_IP_ADDR;
    int addr_family = 0;
    int ip_protocol = 0;

    
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = ((1ULL << 5));
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);    

    // Initialize SPI once up front
    
    spi_master_init();

    while (1) {
#if defined(CONFIG_EXAMPLE_IPV4)
        // struct sockaddr_in dest_addr;
        // inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
        // dest_addr.sin_family = AF_INET;
        // dest_addr.sin_port = htons(PORT);
        // addr_family = AF_INET;
        // ip_protocol = IPPROTO_IP;
#elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
        struct sockaddr_storage dest_addr = { 0 };
        ESP_ERROR_CHECK(get_addr_from_stdin(PORT, SOCK_STREAM, &ip_protocol, &addr_family, &dest_addr));
#endif

        // int sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
        // if (sock < 0) {
        //     ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        //     break;
        // }
        // ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, PORT);

        // int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        // if (err != 0) {
        //     ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
        //     break;
        // }
        // ESP_LOGI(TAG, "Successfully connected");

        // uint8_t i = 0;
        while (1) {
            // 1) Read a block from SPI (blocking call)
            spi_read_bytes();
            
            // gpio_set_level(22,1);
            // vTaskDelay(pdMS_TO_TICKS(1000)); 
            // gpio_set_level(22,0);
            // vTaskDelay(pdMS_TO_TICKS(1000)); 
           
            // // 2) Send the whole block over TCP (blocking until all bytes written)
            // // size_t sent_total = 0;
            // // while (sent_total < SPI_CHUNK_BYTES) {
                
            // //     ssize_t n = send(sock, spi_buf + sent_total,
            // //                      SPI_CHUNK_BYTES - sent_total, 0);
            // //     if (n > 0) {
            // //         sent_total += (size_t)n;
            // //         continue;
            // //     }
            // //     if (n < 0 && (errno == EINTR)) 
            // //     { 
            // //         ESP_LOGE(TAG, "TCP send failed: errno %d", errno);
            // //         break;
            // //     }   
            // // }
            // vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // if (sock != -1) {
        //     ESP_LOGE(TAG, "Shutting down socket and restarting...");
        //     shutdown(sock, 0);
        //     close(sock);
        // }
    }
}
