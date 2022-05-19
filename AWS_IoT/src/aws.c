#include <zephyr.h>
#include <stdio.h>
#include <stdlib.h>
#include <net/aws_iot.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_info.h>
#include <nrf_modem.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <sys/reboot.h>
#include <dfu/mcuboot.h>
#include <date_time.h>
#include "json_support.h"
#include "aws.h"
#include "main.h"

int app_topics_subscribe(void)
{
	int err;
	static char custom_topic[75] = "my-custom-topic/example";
	static char custom_topic_2[75] = "my-custom-topic/example_2";

	const struct aws_iot_topic_data topics_list[APP_TOPICS_COUNT] = {
		[0].str = custom_topic,
		[0].len = strlen(custom_topic),
		[1].str = custom_topic_2,
		[1].len = strlen(custom_topic_2)
	};

	err = aws_iot_subscription_topics_add(topics_list,
					      ARRAY_SIZE(topics_list));
	if (err) {
		printk("aws_iot_subscription_topics_add, error: %d\n", err);
	}

	return err;
}

int shadow_update(bool version_number_include)
{
	int err;
	char *message;
	int64_t message_ts = 0;
	int16_t bat_voltage = 0;

	err = date_time_now(&message_ts);
	if (err) {
		printk("date_time_now, error: %d\n", err);
		return err;
	}

#if defined(CONFIG_NRF_MODEM_LIB)
	/* Request battery voltage data from the modem. */
	err = modem_info_short_get(MODEM_INFO_BATTERY, &bat_voltage);
	if (err != sizeof(bat_voltage)) {
		printk("modem_info_short_get, error: %d\n", err);
		return err;
	}
#endif

	cJSON *root_obj = cJSON_CreateObject();
	cJSON *state_obj = cJSON_CreateObject();
	cJSON *reported_obj = cJSON_CreateObject();

	if (root_obj == NULL || state_obj == NULL || reported_obj == NULL) {
		cJSON_Delete(root_obj);
		cJSON_Delete(state_obj);
		cJSON_Delete(reported_obj);
		err = -ENOMEM;
		return err;
	}

	if (version_number_include) {
		err = json_add_str(reported_obj, "app_version",
				    CONFIG_APP_VERSION);
	} else {
		err = 0;
	}

	err += json_add_number(reported_obj, "batv", bat_voltage);
	err += json_add_number(reported_obj, "ts", message_ts);
	err += json_add_obj(state_obj, "reported", reported_obj);
	err += json_add_obj(root_obj, "state", state_obj);

	if (err) {
		printk("json_add, error: %d\n", err);
		goto cleanup;
	}

	message = cJSON_Print(root_obj);
	if (message == NULL) {
		printk("cJSON_Print, error: returned NULL\n");
		err = -ENOMEM;
		goto cleanup;
	}

	struct aws_iot_data tx_data = {
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic.type = AWS_IOT_SHADOW_TOPIC_UPDATE,
		.ptr = message,
		.len = strlen(message)
	};

	printk("Publishing: %s to AWS IoT broker\n", message);

	err = aws_iot_send(&tx_data);
	if (err) {
		printk("aws_iot_send, error: %d\n", err);
	}

	cJSON_FreeString(message);

cleanup:

	cJSON_Delete(root_obj);

	return err;
}

void print_received_data(const char *buf, const char *topic, size_t topic_len)
{
	char *str = NULL;
	cJSON *root_obj = NULL;

	root_obj = cJSON_Parse(buf);
	if (root_obj == NULL) {
		printk("cJSON Parse failure");
		return;
	}

	str = cJSON_Print(root_obj);
	if (str == NULL) {
		printk("Failed to print JSON object");
		goto clean_exit;
	}

	printf("Data received from AWS IoT console:\nTopic: %.*s\nMessage: %s\n",
	       topic_len, topic, str);

	cJSON_FreeString(str);

clean_exit:
	cJSON_Delete(root_obj);
}

void aws_iot_event_handler(const struct aws_iot_evt *const evt)
{
	switch (evt->type) {
		case AWS_IOT_EVT_CONNECTING:
			printk("AWS_IOT_EVT_CONNECTING\n");
			break;

		case AWS_IOT_EVT_CONNECTED:
			printk("AWS_IOT_EVT_CONNECTED\n");

			cloud_connected = true;
			/* This may fail if the work item is already being processed,
			* but in such case, the next time the work handler is executed,
			* it will exit after checking the above flag and the work will
			* not be scheduled again.
			*/
			(void)k_work_cancel_delayable(&connect_work);

			if (evt->data.persistent_session) {
				printk("Persistent session enabled\n");
			}

	#if defined(CONFIG_NRF_MODEM_LIB)
			/** Successfully connected to AWS IoT broker, mark image as
			 *  working to avoid reverting to the former image upon reboot.
			 */
			boot_write_img_confirmed();
	#endif

			/** Send version number to AWS IoT broker to verify that the
			 *  FOTA update worked.
			 */
			k_work_submit(&shadow_update_version_work);

			/** Start sequential shadow data updates.
			 */
			k_work_schedule(&shadow_update_work,
					K_SECONDS(CONFIG_PUBLICATION_INTERVAL_SECONDS));

	#if defined(CONFIG_NRF_MODEM_LIB)
			int err = lte_lc_psm_req(true);
			if (err) {
				printk("Requesting PSM failed, error: %d\n", err);
			}
	#endif
			break;

		case AWS_IOT_EVT_READY:
			printk("AWS_IOT_EVT_READY\n");
			break;

		case AWS_IOT_EVT_DISCONNECTED:
			printk("AWS_IOT_EVT_DISCONNECTED\n");
			cloud_connected = false;
			/* This may fail if the work item is already being processed,
			* but in such case, the next time the work handler is executed,
			* it will exit after checking the above flag and the work will
			* not be scheduled again.
			*/
			(void)k_work_cancel_delayable(&shadow_update_work);
			k_work_schedule(&connect_work, K_NO_WAIT);
			break;

		case AWS_IOT_EVT_DATA_RECEIVED:
			printk("AWS_IOT_EVT_DATA_RECEIVED\n");
			print_received_data(evt->data.msg.ptr, evt->data.msg.topic.str,
						evt->data.msg.topic.len);
			break;

		case AWS_IOT_EVT_FOTA_START:
			printk("AWS_IOT_EVT_FOTA_START\n");
			break;

		case AWS_IOT_EVT_FOTA_ERASE_PENDING:
			printk("AWS_IOT_EVT_FOTA_ERASE_PENDING\n");
			printk("Disconnect LTE link or reboot\n");
	#if defined(CONFIG_NRF_MODEM_LIB)
			err = lte_lc_offline();
			if (err) {
				printk("Error disconnecting from LTE\n");
			}
	#endif
			break;

		case AWS_IOT_EVT_FOTA_ERASE_DONE:
			printk("AWS_FOTA_EVT_ERASE_DONE\n");
			printk("Reconnecting the LTE link");
	#if defined(CONFIG_NRF_MODEM_LIB)
			err = lte_lc_connect();
			if (err) {
				printk("Error connecting to LTE\n");
			}
	#endif
			break;

		case AWS_IOT_EVT_FOTA_DONE:
			printk("AWS_IOT_EVT_FOTA_DONE\n");
			printk("FOTA done, rebooting device\n");
			aws_iot_disconnect();
			sys_reboot(0);
			break;

		case AWS_IOT_EVT_FOTA_DL_PROGRESS:
			printk("AWS_IOT_EVT_FOTA_DL_PROGRESS, (%d%%)",
				evt->data.fota_progress);

		case AWS_IOT_EVT_ERROR:
			printk("AWS_IOT_EVT_ERROR, %d\n", evt->data.err);
			break;

		case AWS_IOT_EVT_FOTA_ERROR:
			printk("AWS_IOT_EVT_FOTA_ERROR");
			break;

		default:
			printk("Unknown AWS IoT event type: %d\n", evt->type);
			break;
	}
}