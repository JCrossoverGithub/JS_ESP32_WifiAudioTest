// main/main.c
// ESP32: MAX4466 (analog mic) -> ADC continuous (GPIO34 / ADC1_CH6) -> UDP PCM16 mono 16kHz

#include <string.h>
#include <inttypes.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_adc/adc_continuous.h"
#include "driver/adc.h"

static const char *TAG = "udp_audio";

// ====== EDIT THESE ======
#define WIFI_SSID "******"
#define WIFI_PASS "******"

// Your Windows PC IPv4 on the same network (ipconfig -> IPv4 Address)
#define DEST_IP   "192.168.0.xx"
// ========================

#define DEST_PORT 3333

// Audio format
#define SAMPLE_RATE_HZ     20000
#define FRAME_MS           20
#define SAMPLES_PER_FRAME  ((SAMPLE_RATE_HZ * FRAME_MS) / 1000) // 400 samples
#define BYTES_PER_SAMPLE   2
#define UDP_PAYLOAD_BYTES  (SAMPLES_PER_FRAME * BYTES_PER_SAMPLE) // 800 bytes

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

// ADC config: GPIO34 = ADC1_CH6
#define MIC_ADC_UNIT     ADC_UNIT_1
#define MIC_ADC_CHANNEL  ADC_CHANNEL_6       // GPIO34
#define MIC_ADC_ATTEN    ADC_ATTEN_DB_11     // widest range
#define MIC_ADC_BITWIDTH ADC_BITWIDTH_12

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 10) {
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/10)...", s_retry_num);
            ESP_ERROR_CHECK(esp_wifi_connect());
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    // Reduce wifi signal strength to prevent microphone pickup interference / humming.
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(40));

    // Reduce jitter for streaming (optional but helps)
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID=%s ...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        portMAX_DELAY
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected.");
    } else {
        ESP_LOGE(TAG, "Wi-Fi failed to connect.");
    }
}

// Very small DC remover (IIR) + scale to int16 PCM
static inline int16_t adc_to_pcm16(uint16_t raw, int32_t *dc_est) {
    // IIR DC estimate: dc += (raw - dc)/1024  (cheap and stable)
    *dc_est += ((int32_t)raw - *dc_est) >> 10;
    int32_t centered = (int32_t)raw - *dc_est;   // around 0
    int32_t s = centered << 4;                   // 12-bit -> ~16-bit
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static void udp_audio_task(void *arg) {
    (void)arg;

    // UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DEST_PORT);
    dest.sin_addr.s_addr = inet_addr(DEST_IP);

    ESP_LOGI(TAG, "UDP streaming to %s:%d (PCM16 mono %d Hz)", DEST_IP, DEST_PORT, SAMPLE_RATE_HZ);

    // ADC continuous setup
    adc_continuous_handle_t adc_handle = NULL;

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 8192,
        .conv_frame_size = 2048, // bytes returned per read (approx)
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    adc_digi_pattern_config_t pattern = {
        .atten = MIC_ADC_ATTEN,
        .channel = MIC_ADC_CHANNEL,
        .unit = MIC_ADC_UNIT,
        .bit_width = MIC_ADC_BITWIDTH,
    };

    adc_continuous_config_t adc_cfg = {
        .sample_freq_hz = SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
        .pattern_num = 1,
        .adc_pattern = &pattern,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &adc_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

    // Packet format: [u32 seq][PCM16...]
    uint8_t packet[4 + UDP_PAYLOAD_BYTES];
    uint32_t seq = 0;

    int32_t dc_est = 2048; // start near mid-scale
    int16_t frame[SAMPLES_PER_FRAME];
    int frame_fill = 0;

    uint8_t adc_buf[2048];
    uint32_t bytes_read = 0;

    while (1) {
        esp_err_t r = adc_continuous_read(
            adc_handle,
            adc_buf,
            sizeof(adc_buf),
            &bytes_read,
            1000  // ms
        );
        if (r != ESP_OK) continue;

        const adc_digi_output_data_t *out = (const adc_digi_output_data_t *)adc_buf;
        int n = bytes_read / sizeof(adc_digi_output_data_t);

        for (int i = 0; i < n; i++) {
            // TYPE1 format (ADC1): channel + 12-bit data
            uint16_t raw = out[i].type1.data;

            frame[frame_fill++] = adc_to_pcm16(raw, &dc_est);

            if (frame_fill == SAMPLES_PER_FRAME) {
                // write seq
                packet[0] = (uint8_t)(seq & 0xFF);
                packet[1] = (uint8_t)((seq >> 8) & 0xFF);
                packet[2] = (uint8_t)((seq >> 16) & 0xFF);
                packet[3] = (uint8_t)((seq >> 24) & 0xFF);

                // copy PCM payload
                memcpy(&packet[4], frame, UDP_PAYLOAD_BYTES);

                int sent = sendto(sock, packet, sizeof(packet), 0,
                                  (struct sockaddr *)&dest, sizeof(dest));
                if (sent < 0) {
                    ESP_LOGW(TAG, "sendto failed");
                }

                seq++;
                frame_fill = 0;
            }
        }
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_sta();

    xTaskCreatePinnedToCore(udp_audio_task, "udp_audio", 8192, NULL, 5, NULL, 0);

    // Keep app_main alive (optional)
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
