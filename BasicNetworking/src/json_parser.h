#include "cJSON.h"
#include "cJSON_os.h"

struct open_weather_api_return {
    float temperature;
    int pressure;
    int humidity;
    int time;
};

struct open_weather_api_return get_temp_pressure_humidity(const char *json_string);
