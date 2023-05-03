/* 
 * Copyright (c) 2023 Tai-Jie(Denny) Yun.
 * 
 * API Information:
 * https://docs.zephyrproject.org/3.2.0/connectivity/networking/api/http.html
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "json_parser.h"

struct open_weather_api_return get_temp_pressure_humidity(const char *json_string)
{
    struct open_weather_api_return result;
	cJSON *root = cJSON_Parse(json_string);
    cJSON *main_obj = cJSON_GetObjectItemCaseSensitive(root, "main");

    float temp = cJSON_GetObjectItemCaseSensitive(main_obj, "temp")->valuedouble;
    int pressure = cJSON_GetObjectItemCaseSensitive(main_obj, "pressure")->valueint;
    int humidity = cJSON_GetObjectItemCaseSensitive(main_obj, "humidity")->valueint;

    int time = cJSON_GetObjectItemCaseSensitive(root, "dt")->valueint;;

    result.temperature=temp;
    result.pressure=pressure;
    result.humidity=humidity;

    result.time=time;
    
    cJSON_Delete(root);

    return result;
}