
#include <zephyr/kernel.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <date_time.h>
#include <zephyr/posix/time.h>
#include <zephyr/posix/sys/time.h>
#include "http_get.h"
#include "json_parser.h"

#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <lvgl.h>

static uint32_t count;

struct open_weather_api_return http_result;
int sock;
lv_obj_t *hello_world_label;

static const int pwm_fequency = 500; // in Hz
static float input_period = (float)(1.0 / pwm_fequency) * 1000000000.0;
static int pwm_duty_cycle = 80; // in percentage

static const struct pwm_dt_spec pwm_lcd_backlight = PWM_DT_SPEC_GET(DT_ALIAS(lcd_led1));

static struct gpio_dt_spec button_gpio = GPIO_DT_SPEC_GET_OR(
		DT_ALIAS(sw0), gpios, {0});
static struct gpio_callback button_callback;

/* Define thread stack size and priority*/
#define MY_STACK_SIZE 16384
#define MY_PRIORITY 5

K_THREAD_STACK_DEFINE(my_stack_area, MY_STACK_SIZE);

void data_work_handler(struct k_work *work);
K_WORK_DEFINE(data_action_work, data_work_handler);

struct k_work_q my_work_q;

void data_work_handler(struct k_work *work) {
  	char text[100];
	struct addrinfo *res;
	nslookup("api.openweathermap.org", &res);
	printk("Connecting to HTTP Server:\n");
    sock = connect_socket(&res);
    http_result = http_get(sock, "api.openweathermap.org", "/data/2.5/weather?q=Adelaide&appid=807031fe9f3e3282667deb969fbdff57&units=metric");
    zsock_close(sock);
    
    printf("Temperature: %f degree Celsius\nPressure: %d hPa\nHumidity: %d%%\n", http_result.temperature, http_result.pressure, http_result.humidity);
	sprintf(text, "Temperature: %.2f\nTime: %d", http_result.temperature, http_result.time);
	lv_label_set_text(hello_world_label, text);
}

static void button_isr_callback(const struct device *port,
				struct gpio_callback *cb,
				uint32_t pins)
{
	k_work_submit_to_queue(&my_work_q, &data_action_work); // call the breath and warning algorithm
	
	count = 0;
}

void print_modem_info(enum modem_info info)
{
	int len;
	char buf[80];

	switch (info) {
		case MODEM_INFO_RSRP:
			printk("Signal Strength: ");
			break;
		case MODEM_INFO_IP_ADDRESS:
			printk("IP Addr: ");
			break;
		case MODEM_INFO_FW_VERSION:
			printk("Modem FW Ver: ");
			break;
		case MODEM_INFO_ICCID:
			printk("SIM ICCID: ");
			break;
		case MODEM_INFO_IMSI:
			printk("IMSI: ");
			break;
		case MODEM_INFO_IMEI:
			printk("IMEI: ");
			break;
		case MODEM_INFO_DATE_TIME:
			printk("Network Date/Time: ");
			break;
		case MODEM_INFO_APN:
			printk("APN: ");
			break;
		default:
			printk("Unsupported: ");
			break;
	}

	len = modem_info_string_get(info, buf, 80);
	if (len > 0) {
		printk("%s\n",buf);
	} else {
		printk("Error\n");
	}
}

void main(void)
{
	int err;

	char count_str[11] = {0};	
	const struct device *display_dev;

	lv_obj_t *count_label;

	printk("\nnRF9160 Basic Networking Example (%s)\n", CONFIG_BOARD);

	if (!device_is_ready(pwm_lcd_backlight.dev)) {
		printk("Error: PWM device %s is not ready\n", pwm_lcd_backlight.dev->name);
		return;
	}	
	
    pwm_set_dt(&pwm_lcd_backlight, input_period, input_period*pwm_duty_cycle/100); 

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		printk("Device not ready, aborting test");
		return;
	}

	if (device_is_ready(button_gpio.port)) {
		err = gpio_pin_configure_dt(&button_gpio, GPIO_INPUT);
		if (err) {
			printk("failed to configure button gpio: %d", err);
			return;
		}

		gpio_init_callback(&button_callback, button_isr_callback,
				   BIT(button_gpio.pin));

		err = gpio_add_callback(button_gpio.port, &button_callback);
		if (err) {
			printk("failed to add button callback: %d", err);
			return;
		}

		err = gpio_pin_interrupt_configure_dt(&button_gpio,
						      GPIO_INT_EDGE_TO_ACTIVE);
		if (err) {
			printk("failed to enable button callback: %d", err);
			return;
		}
	}

	err = lte_lc_init();
	if (err) printk("MODEM: Failed initializing LTE Link controller, error: %d\n", err);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_UICC);
	if (err) printk("MODEM: Failed enabling UICC power, error: %d\n", err);
	k_msleep(100);
	
	err = modem_info_init();
	if (err) printk("MODEM: Failed initializing modem info module, error: %d\n", err);

	print_modem_info(MODEM_INFO_FW_VERSION);
	print_modem_info(MODEM_INFO_IMEI);
	print_modem_info(MODEM_INFO_ICCID);

	printk("Waiting for network... ");
	err = lte_lc_init_and_connect();
	if (err) {
		printk("Failed to connect to the LTE network, err %d\n", err);
		return;
	}
	printk("OK\n");
	print_modem_info(MODEM_INFO_APN);
	print_modem_info(MODEM_INFO_IP_ADDRESS);
	print_modem_info(MODEM_INFO_RSRP);

	// struct addrinfo *res;
	// nslookup("api.openweathermap.org", &res);
	// print_addrinfo_results(&res);

	k_work_queue_init(&my_work_q);

    k_work_queue_start(&my_work_q, my_stack_area,
                      K_THREAD_STACK_SIZEOF(my_stack_area), MY_PRIORITY,
                      NULL);

	lv_disp_t * dispp = lv_disp_get_default();
    lv_theme_t * theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                                               true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);
	
	hello_world_label = lv_label_create(lv_scr_act());
	
	lv_label_set_text(hello_world_label, "Hello world!\nWelcome to Lemon IoT");
	lv_obj_align(hello_world_label, LV_ALIGN_CENTER, 0, 0);

	count_label = lv_label_create(lv_scr_act());
	lv_obj_align(count_label, LV_ALIGN_BOTTOM_MID, 0, 0);

	lv_task_handler();
	display_blanking_off(display_dev);

	while (1) {
		if ((count % 100) == 0U) {
			sprintf(count_str, "%d", count/100U);
			lv_label_set_text(count_label, count_str);
		}
		lv_task_handler();
		++count;
		k_sleep(K_MSEC(10));
	}

}


