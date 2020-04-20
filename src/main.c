/*
 * Copyright 2018 David B Brown (@maccoylton)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Homekit firmware SWA9 SmartSocket
 */

#define DEVICE_MANUFACTURER "Gosund"
#define DEVICE_NAME "Smartplug"
#define DEVICE_MODEL "UP111"
#define DEVICE_SERIAL "12345678"
#define FW_VERSION "1.0"
#define SAVE_DELAY 500
#define POWER_MONITOR_POLL_PERIOD 10000


#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <wifi_config.h>


#include <adv_button.h>
#include <led_codes.h>
#include <udplogger.h>
#include <custom_characteristics.h>
#include <shared_functions.h>
#include <HLW8012_ESP82.h>




// add this section to make your device OTA capable
// create the extra characteristic &ota_trigger, at the end of the primary service (before the NULL)
// it can be used in Eve, which will show it, where Home does not
// and apply the four other parameters in the accessories_information section

#include <ota-api.h>

void switch_on_callback(homekit_characteristic_t *_ch, homekit_value_t on, void *context);
void volts_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context);
void amps_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context);
void watts_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context);
void calibrate_pow_set (homekit_value_t value);
void calibrate_volts_set (homekit_value_t value);
void calibrate_power_set (homekit_value_t value);

homekit_characteristic_t wifi_reset   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_RESET, false, .setter=wifi_reset_set);
homekit_characteristic_t wifi_check_interval   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_CHECK_INTERVAL, 10, .setter=wifi_check_interval_set);
/* checks the wifi is connected and flashes status led to indicated connected */
homekit_characteristic_t task_stats   = HOMEKIT_CHARACTERISTIC_(CUSTOM_TASK_STATS, false , .setter=task_stats_set);
homekit_characteristic_t ota_beta     = HOMEKIT_CHARACTERISTIC_(CUSTOM_OTA_BETA, false, .setter=ota_beta_set);
homekit_characteristic_t lcm_beta    = HOMEKIT_CHARACTERISTIC_(CUSTOM_LCM_BETA, false, .setter=lcm_beta_set);


homekit_characteristic_t calibrate_pow    = HOMEKIT_CHARACTERISTIC_(CUSTOM_CALIBRATE_POW, false, .setter=calibrate_pow_set);
homekit_characteristic_t calibrate_volts   = HOMEKIT_CHARACTERISTIC_(CUSTOM_CALIBRATE_VOLTS, 230, .setter=calibrate_volts_set);
homekit_characteristic_t calibrate_power    = HOMEKIT_CHARACTERISTIC_(CUSTOM_CALIBRATE_WATTS, 60, .setter=calibrate_power_set);


homekit_characteristic_t ota_trigger  = API_OTA_TRIGGER;
homekit_characteristic_t name         = HOMEKIT_CHARACTERISTIC_(NAME, DEVICE_NAME);
homekit_characteristic_t manufacturer = HOMEKIT_CHARACTERISTIC_(MANUFACTURER,  DEVICE_MANUFACTURER);
homekit_characteristic_t serial       = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, DEVICE_SERIAL);
homekit_characteristic_t model        = HOMEKIT_CHARACTERISTIC_(MODEL,         DEVICE_MODEL);
homekit_characteristic_t revision     = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION,  FW_VERSION);
homekit_characteristic_t switch_on = HOMEKIT_CHARACTERISTIC_(
                                                             ON, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(switch_on_callback)
                                                             );
homekit_characteristic_t volts = HOMEKIT_CHARACTERISTIC_(
                                                         CUSTOM_VOLTS, 0, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(volts_callback)
                                                         );
homekit_characteristic_t amps = HOMEKIT_CHARACTERISTIC_(
                                                        CUSTOM_AMPS, 0, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(amps_callback)
                                                        );
homekit_characteristic_t watts = HOMEKIT_CHARACTERISTIC_(
                                                         CUSTOM_WATTS, 0, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(watts_callback)
                                                         );

TaskHandle_t power_monitoring_task_handle;

// The GPIO pin that is connected to the relay on the SWA9.
const int relay_gpio = 14;
// The GPIO pin that is connected to the LED on the SWA9.
const int LED_GPIO = 1;
// The GPIO pin that is oconnected to the button on the SWA9.
/* const int reset_button_gpio = 0; */
const int BUTTON_GPIO = 3;
int led_off_value=1; /* global varibale to support LEDs set to 0 where the LED is connected to GND, 1 where +3.3v */

const int status_led_gpio = 13; /*set the gloabl variable for the led to be sued for showing status */

const int CF_GPIO = 4;
const int CF1_GPIO = 5;
const int SELi_GPIO = 12;

float calibrated_volts_multiplier=0;
float calibrated_current_multiplier=0;
float calibrated_power_multiplier=0;

void s2_button_callback(uint8_t gpio, void* args, const uint8_t param) {
    
    printf("Button event single press on GPIO : %d\n", gpio);
    printf("Toggling relay\n");
    switch_on.value.bool_value = !switch_on.value.bool_value;
    relay_write(switch_on.value.bool_value, relay_gpio);
    led_write(switch_on.value.bool_value, LED_GPIO);
    homekit_characteristic_notify(&switch_on, switch_on.value);
    sdk_os_timer_arm (&save_timer, SAVE_DELAY, 0 );
}

void gpio_init() {
    
    
    adv_button_set_evaluate_delay(10);
    
    /* GPIO for button, pull-up resistor, inverted */
    printf("Initialising buttons\n");
    adv_button_create(BUTTON_GPIO, true, false);
    adv_button_register_callback_fn(BUTTON_GPIO, s2_button_callback, SINGLEPRESS_TYPE, NULL, 0);
    adv_button_register_callback_fn(BUTTON_GPIO, reset_button_callback, VERYLONGPRESS_TYPE, NULL, 0);
    
    
    gpio_enable(LED_GPIO, GPIO_OUTPUT);
    led_write(false, LED_GPIO);
    gpio_enable(relay_gpio, GPIO_OUTPUT);
    relay_write(switch_on.value.bool_value, relay_gpio);
    
}

void switch_on_callback(homekit_characteristic_t *_ch, homekit_value_t on, void *context) {
    printf("Switch on callback\n");
    relay_write(switch_on.value.bool_value, relay_gpio);
    led_write(switch_on.value.bool_value, LED_GPIO);
}

void volts_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    printf("Volts on callback\n");
/*    volts.value.int_value = HLW8012_getVoltage();*/
}


void amps_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    printf("Amps on callback\n");
/*    amps.value.float_value = HLW8012_getCurrent();*/
}


void watts_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    printf("Watts on callback\n");
/*    watts.value.int_value = HLW8012_getActivePower();*/
/*    watts.value.int_value = (int) (volts.value.int_value * amps.value.float_value);*/
    
}


void calibrate_task (){
    
    HLW8012_set_calibrated_mutipliers (&calibrated_current_multiplier, &calibrated_volts_multiplier, &calibrated_power_multiplier, calibrate_volts.value.int_value, calibrate_power.value.int_value) ;
    
    sdk_os_timer_arm (&save_timer, SAVE_DELAY, 0 );
    
    calibrate_pow.value.bool_value = false;
    homekit_characteristic_notify(&calibrate_pow, calibrate_pow.value);

    vTaskDelete (NULL);
}

void calibrate_pow_set (homekit_value_t value) {

    xTaskCreate(calibrate_task, "Calibrate task", 512, NULL, tskIDLE_PRIORITY+1, NULL);
}


void calibrate_volts_set (homekit_value_t value) {
    
    calibrate_volts.value.int_value = value.int_value;
}


void calibrate_power_set (homekit_value_t value) {
    
    calibrate_power.value.int_value = value.int_value;
}


homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_switch, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            &name,
            &manufacturer,
            &serial,
            &model,
            &revision,
            HOMEKIT_CHARACTERISTIC(IDENTIFY, identify),
            NULL
        }),
        HOMEKIT_SERVICE(SWITCH, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Smartplug"),
            &switch_on,
            &volts,
            &watts,
            &amps,
            &ota_trigger,
            &wifi_reset,
            &wifi_check_interval,
            &task_stats,
            &ota_beta,
            &lcm_beta,
            &calibrate_pow,
            &calibrate_volts,
            &calibrate_power,
            NULL
        }),
        NULL
    }),
    NULL
};

void power_monitoring_task(void *_args) {

    printf ("%s:\n", __func__);

    while (1)
    {
        volts.value.int_value = HLW8012_getVoltage();
        amps.value.float_value = HLW8012_getCurrent();
        /*watts.value.int_value = HLW8012_getActivePower();*/
        watts.value.int_value = (int) (volts.value.int_value * amps.value.float_value);
        
        printf("%s: [HLW] Active Power (W)    :%d\n", __func__, HLW8012_getActivePower());
        printf("%s: [HLW] Voltage (V)         :%d\n", __func__, HLW8012_getVoltage());
        printf("%s: [HLW] Current (A)         :%f\n", __func__, HLW8012_getCurrent());
        printf("%s: [HLW] Apparent Power (VA) :%d\n", __func__, HLW8012_getApparentPower());
        printf("%s: [HLW] Power Factor (%%)    :%d\n", __func__, (int) (100 * HLW8012_getPowerFactor()));
        printf("%s: [HLW] Agg. energy (Ws)    :%d\n", __func__, HLW8012_getEnergy());
        
        homekit_characteristic_bounds_check( &volts);
        homekit_characteristic_bounds_check( &amps);
        homekit_characteristic_bounds_check( &watts);
        
        homekit_characteristic_notify(&volts, volts.value);
        homekit_characteristic_notify(&amps, amps.value);
        homekit_characteristic_notify(&watts, watts.value);
        vTaskDelay(POWER_MONITOR_POLL_PERIOD / portTICK_PERIOD_MS);
    }
    
}


void recover_from_reset (int reason) {
/* called if we restarted abnormally */
    printf ("%s: reason %d\n", __func__, reason);
    load_characteristic_from_flash(&switch_on);
    homekit_characteristic_notify(&switch_on, switch_on.value);
}


void save_characteristics (  ) {
/* called by a timer function to save charactersitics */
    printf ("%s:\n", __func__);
    save_characteristic_to_flash(&switch_on, switch_on.value);
    save_float_param ( "wattsx", calibrated_power_multiplier);
    save_float_param ( "voltsx", calibrated_volts_multiplier);
    save_float_param ( "currentx", calibrated_current_multiplier);

}


void accessory_init_not_paired (void) {
    /* initalise anything you don't want started until wifi and homekit imitialisation is confirmed, but not paired */
}


void accessory_init (void ){
    /* initalise anything you don't want started until wifi and pairing is confirmed */
    printf ("%s:Call HLW8012_init\n", __func__);
    HLW8012_init(CF_GPIO, CF_GPIO, SELi_GPIO, 1, 0);
    // currentWhen  - 1 for HLW8012 (old Sonoff Pow), 0 for BL0937
    // model - 0 for HLW8012, 1 or other value for BL0937
    
    load_float_param ( "wattsx", &calibrated_power_multiplier);
    load_float_param ( "voltsx", &calibrated_volts_multiplier);
    load_float_param ( "currentx", &calibrated_current_multiplier);
    
    if ( calibrated_power_multiplier !=0 && calibrated_current_multiplier !=0 && calibrated_volts_multiplier !=0){
        printf ("%s:Setting calibrated multipliers, current: %f, voltage: %f, watts: %f\n", __func__, calibrated_current_multiplier, calibrated_volts_multiplier, calibrated_power_multiplier);
        HLW8012_setCurrentMultiplier(calibrated_current_multiplier);
        HLW8012_setVoltageMultiplier(calibrated_volts_multiplier);
        HLW8012_setPowerMultiplier(calibrated_power_multiplier);
    } else {
        printf ("%s:calibrated mutipliers not available, current: %f, voltage: %f, watts: %f\n", __func__, calibrated_current_multiplier, calibrated_volts_multiplier, calibrated_power_multiplier);
    }
    
    xTaskCreate(power_monitoring_task, "Power Monitoring Task", 512, NULL, tskIDLE_PRIORITY+1, &power_monitoring_task_handle);

    homekit_characteristic_notify(&switch_on, switch_on.value);
    
}


homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .setupId = "1234",
    .on_event = on_homekit_event
};


void user_init(void) {
    
    standard_init (&name, &manufacturer, &model, &serial, &revision);
    
    gpio_init();
    
    
    wifi_config_init(DEVICE_NAME, NULL, on_wifi_ready);
    
}
