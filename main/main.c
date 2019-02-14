#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

#include "driver/gpio.h"
#include "sdkconfig.h"

#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/api.h"

#include "led_strip/led_strip.h"

/* Can run 'make menuconfig' to choose the GPIO to blink,
   or you can edit the following line and set a number here.
*/

#define BLINK_GPIO_1 32
#define BLINK_GPIO_2 33
#define BLINK_TIME 300

// WIFI configuration
#define AP_SSID "ESP_32"
#define AP_PASSPHARSE "12345678"
#define AP_SSID_HIDDEN 0
#define AP_MAX_CONNECTIONS 10
#define AP_AUTHMODE WIFI_AUTH_WPA2_PSK // the passpharese should be atleast 8 chars long
#define AP_BEACON_INTERVAL 200         // in milli seconds

static EventGroupHandle_t wifi_event_group;

static SemaphoreHandle_t mutex = NULL;
static uint64_t total_red_counter = 0;
static uint64_t total_blue_counter = 0;
static uint8_t led_counter = 11;

#define LED_STRIP_LENGTH 22U
static struct led_color_t led_strip_buf_1[LED_STRIP_LENGTH];
static struct led_color_t led_strip_buf_2[LED_STRIP_LENGTH];

#define LED_STRIP_RMT_INTR_NUM 19

const int CLIENT_CONNECTED_BIT = BIT0;
const int CLIENT_DISCONNECTED_BIT = BIT1;
const int AP_STARTED_BIT = BIT2;
const int ACTIVATE_GPIO_BIT = BIT3;

const static char http_html_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n Hola desde ESP32! :) \n";
const static char http_html_hdr_ok[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_err_t event_handler(void *ctx, system_event_t *event) {
	switch (event->event_id) {
	case SYSTEM_EVENT_AP_START:
		ESP_LOGI("WiFi", "Started WiFi in AP mode.");
		xEventGroupSetBits(wifi_event_group, AP_STARTED_BIT);
		break;

	case SYSTEM_EVENT_AP_STACONNECTED:
		ESP_LOGI("WiFi", "New station connected.");
		xEventGroupSetBits(wifi_event_group, CLIENT_CONNECTED_BIT);
		break;

	case SYSTEM_EVENT_AP_STADISCONNECTED:
		ESP_LOGI("WiFi", "A station disconnected.");
		xEventGroupSetBits(wifi_event_group, CLIENT_DISCONNECTED_BIT);
		break;
	default:
		break;
	}
	return ESP_OK;
}

static void start_dhcp_server(void) {
	// initialize the tcp stack
	tcpip_adapter_init();
	// stop DHCP server
	ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
	// assign a static IP to the network interface
	tcpip_adapter_ip_info_t info;
	memset(&info, 0, sizeof(info));
	IP4_ADDR(&info.ip, 192, 168, 2, 1);
	IP4_ADDR(&info.gw, 192, 168, 2, 1); // ESP acts as router, so gw addr will be its own addr
	IP4_ADDR(&info.netmask, 255, 255, 255, 0);
	ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info));
	// start the DHCP server
	ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
	ESP_LOGI("DHCP_Status", "DHCP server started.");
}

void set_gpio_configuration(void) {
	gpio_pad_select_gpio(BLINK_GPIO_1);
	gpio_pad_select_gpio(BLINK_GPIO_2);
	// Set the GPIO as a push/pull output
	gpio_set_direction(BLINK_GPIO_1, GPIO_MODE_OUTPUT);
	gpio_set_direction(BLINK_GPIO_2, GPIO_MODE_OUTPUT);

	// Set initial status = OFF
	gpio_set_level(BLINK_GPIO_1, 0);
	gpio_set_level(BLINK_GPIO_2, 0);
}

static void start_wifi_ap_mode(void) {
	esp_log_level_set("wifi", ESP_LOG_NONE); // disable wifi driver logging

	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
	wifi_event_group = xEventGroupCreate();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

	// configure the wifi connection and start the interface
	wifi_config_t ap_config = {
	.ap =
	{
	.ssid = AP_SSID,
	.password = AP_PASSPHARSE,
	.ssid_len = 0,
	.channel = 0,
	.authmode = AP_AUTHMODE,
	.ssid_hidden = AP_SSID_HIDDEN,
	.max_connection = AP_MAX_CONNECTIONS,
	.beacon_interval = AP_BEACON_INTERVAL,
	},
	};
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
	ESP_ERROR_CHECK(esp_wifi_start());
}

// print the list of connected stations
void printStationList() {
	printf(" Connected stations:\n");
	printf("--------------------------------------------------\n");

	wifi_sta_list_t wifi_sta_list;
	tcpip_adapter_sta_list_t adapter_sta_list;

	memset(&wifi_sta_list, 0, sizeof(wifi_sta_list));
	memset(&adapter_sta_list, 0, sizeof(adapter_sta_list));

	ESP_ERROR_CHECK(esp_wifi_ap_get_sta_list(&wifi_sta_list));
	ESP_ERROR_CHECK(tcpip_adapter_get_sta_list(&wifi_sta_list, &adapter_sta_list));

	for (int i = 0; i < adapter_sta_list.num; i++) {
		tcpip_adapter_sta_info_t station = adapter_sta_list.sta[i];
		printf("%d - mac: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x - IP: %s\n", i + 1, station.mac[0], station.mac[1],
		       station.mac[2], station.mac[3], station.mac[4], station.mac[5], ip4addr_ntoa(&(station.ip)));
	}
	printf("\n");
}

void sta_info(void *pvParam) {

	set_gpio_configuration();

	ESP_LOGI("Connection_Status_Info", "print_sta_info task started \n");
	while (1) {
		EventBits_t staBits = xEventGroupWaitBits(wifi_event_group, CLIENT_CONNECTED_BIT | CLIENT_DISCONNECTED_BIT,
		                                          pdTRUE, pdFALSE, portMAX_DELAY);
		if ((staBits & CLIENT_CONNECTED_BIT) != 0) {
			/* Blink on (output high) */
			gpio_set_level(BLINK_GPIO_1, 1);
			vTaskDelay(BLINK_TIME / portTICK_PERIOD_MS);

			/* Blink off (output low) */
			gpio_set_level(BLINK_GPIO_1, 0);
			vTaskDelay(BLINK_TIME / portTICK_PERIOD_MS);
		} else {
			/* Blink on (output high) */
			gpio_set_level(BLINK_GPIO_2, 1);
			vTaskDelay(BLINK_TIME / portTICK_PERIOD_MS);

			/* Blink off (output low) */
			gpio_set_level(BLINK_GPIO_2, 0);
			vTaskDelay(BLINK_TIME / portTICK_PERIOD_MS);
		}
		printStationList();
	}
}

static void http_server_netconn_serve(struct netconn *conn) {

	struct netbuf *inbuf;
	char *buf;
	u16_t buflen;
	err_t err;

	err = netconn_recv(conn, &inbuf);

	if (err == ERR_OK) {

		netbuf_data(inbuf, (void **) &buf, &buflen);

		// extract the first line, with the request
		char *first_line = strtok(buf, "\n");

		if (first_line) {

			// default page
			if (strstr(first_line, "GET / ")) {
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);

				char *buffer = NULL;

				xSemaphoreTake(mutex, portMAX_DELAY);
				uint64_t red = total_red_counter;
				uint64_t blue = total_blue_counter;
				xSemaphoreGive(mutex);

				uint64_t tot = (red + blue);

				asprintf(&buffer, "<br> Score: <br> RED: %llu = %f <br> BLUE: %llu = %f <br> <br> Players: <br> ", red,
				         (float) red / tot, blue, (float) blue / tot);

				netconn_write(conn, buffer, strlen(buffer) - 1, NETCONN_COPY);
				free(buffer);

				wifi_sta_list_t wifi_sta_list;
				tcpip_adapter_sta_list_t adapter_sta_list;

				memset(&wifi_sta_list, 0, sizeof(wifi_sta_list));
				memset(&adapter_sta_list, 0, sizeof(adapter_sta_list));

				ESP_ERROR_CHECK(esp_wifi_ap_get_sta_list(&wifi_sta_list));
				ESP_ERROR_CHECK(tcpip_adapter_get_sta_list(&wifi_sta_list, &adapter_sta_list));

				static char br[] = "<br>";
				netconn_write(conn, br, sizeof(br) - 1, NETCONN_NOCOPY);

				for (int i = 0; i < adapter_sta_list.num; i++) {
					memset(&buffer, 0, sizeof(buffer));
					tcpip_adapter_sta_info_t station = adapter_sta_list.sta[i];
					asprintf(&buffer, "%d - mac: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x - IP: %s\n", i + 1, station.mac[0],
					         station.mac[1], station.mac[2], station.mac[3], station.mac[4], station.mac[5],
					         ip4addr_ntoa(&(station.ip)));

					netconn_write(conn, buffer, strlen(buffer) - 1, NETCONN_COPY);
					free(buffer);
					netconn_write(conn, br, sizeof(br) - 1, NETCONN_NOCOPY);
				}

			} else if (strstr(first_line, "GET /bear ")) {
				netconn_write(conn, http_html_hdr_ok, sizeof(http_html_hdr_ok), NETCONN_NOCOPY);
				netconn_write(conn, index_html_start, index_html_end - index_html_start, NETCONN_NOCOPY);
			} else if (strstr(first_line, "POST /red ")) {
				xSemaphoreTake(mutex, portMAX_DELAY);
				total_red_counter++;
				if (led_counter < LED_STRIP_LENGTH) {
					led_counter++;
				}
				xSemaphoreGive(mutex);
				ESP_LOGI("HTTP Server", "Got red ...");
			} else if (strstr(first_line, "POST /blue ")) {
				xSemaphoreTake(mutex, portMAX_DELAY);
				total_blue_counter++;
				if (led_counter > 0) {
					led_counter--;
				}
				xSemaphoreGive(mutex);
				ESP_LOGI("HTTP Server", "Got blue ...");
			} else if (!strstr(first_line, "GET /favicon.ico "))
				printf("Unkown request: %s\n", first_line);

		} else
			printf("Unkown request\n");
	}

	// close the connection and free the buffer
	netconn_close(conn);
	netbuf_delete(inbuf);
}

static void http_server(void *pvParameters) {

	struct netconn *conn, *newconn;
	err_t err;
	conn = netconn_new(NETCONN_TCP);
	netconn_bind(conn, NULL, 80);
	netconn_listen(conn);
	ESP_LOGI("HTTP Server", "listening...");
	do {
		err = netconn_accept(conn, &newconn);
		ESP_LOGI("HTTP Server", "New client connected");

		if (err == ERR_OK) {
			http_server_netconn_serve(newconn);
			netconn_delete(newconn);
		}
		vTaskDelay(1); // allows task to be pre-empted
	} while (err == ERR_OK);
	netconn_close(conn);
	netconn_delete(conn);
}

void app_main() {
	/* Print chip information */
	esp_chip_info_t chip_info;
	esp_chip_info(&chip_info);
	printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ", chip_info.cores,
	       (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "", (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
	printf("silicon revision %d, ", chip_info.revision);
	printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
	       (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

	mutex = xSemaphoreCreateMutex();

	if (mutex == NULL)
		ESP_LOGE("Mutex", "Not created ...");

	// Initialize NVS
	// Used to store configuration parameters in flash
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}

	start_dhcp_server();
	start_wifi_ap_mode();

	// Get MAC and IP and activate GPIOs
	// xTaskCreate(&sta_info, "print_sta_info", 2048, NULL, 5, NULL);

	// start the HTTP Server task
	xTaskCreate(&http_server, "http_server", 4096, NULL, 10, NULL);

	// Display Score Leds
	struct led_strip_t led_strip = {.rgb_led_type = RGB_LED_TYPE_WS2812,
	                                .rmt_channel = RMT_CHANNEL_1,
	                                .rmt_interrupt_num = LED_STRIP_RMT_INTR_NUM,
	                                .gpio = GPIO_NUM_22,
	                                .led_strip_buf_1 = led_strip_buf_1,
	                                .led_strip_buf_2 = led_strip_buf_2,
	                                .led_strip_length = LED_STRIP_LENGTH};
	led_strip.access_semaphore = xSemaphoreCreateMutex();

	bool led_init_ok = led_strip_init(&led_strip);
	assert(led_init_ok);

	struct led_color_t led_red_color = {
	.red = 5, .green = 0, .blue = 0,
	};

	struct led_color_t led_blue_color = {
	.red = 0, .green = 0, .blue = 5,
	};

	ESP_LOGI("LEDS", "started ...");

	while (true) {

		xSemaphoreTake(mutex, portMAX_DELAY);
		int split_point = led_counter;
		xSemaphoreGive(mutex);

		for (uint32_t index = 0; index < LED_STRIP_LENGTH; index++) {
			if (index < split_point) {
				led_strip_set_pixel_color(&led_strip, index, &led_red_color);

			} else {
				led_strip_set_pixel_color(&led_strip, index, &led_blue_color);
			}
		}

		led_strip_show(&led_strip);
		vTaskDelay(30 / portTICK_RATE_MS);
	}
}
