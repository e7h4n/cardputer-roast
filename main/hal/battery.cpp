#include "hal.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <algorithm>

namespace {
adc_oneshot_unit_handle_t adc;
adc_cali_handle_t calibration;
adc_channel_t channel;
bool initialized;
}

void Hal::battery_init()
{
    adc_unit_t unit;
    esp_err_t err=adc_oneshot_io_to_channel(10,&unit,&channel);
    if(err==ESP_OK){
        adc_oneshot_unit_init_cfg_t cfg={};cfg.unit_id=unit;
        err=adc_oneshot_new_unit(&cfg,&adc);
    }
    if(err==ESP_OK){
        adc_oneshot_chan_cfg_t cfg={};cfg.atten=ADC_ATTEN_DB_12;cfg.bitwidth=ADC_BITWIDTH_12;
        err=adc_oneshot_config_channel(adc,channel,&cfg);
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if(err==ESP_OK){
        adc_cali_curve_fitting_config_t cfg={};cfg.unit_id=unit;cfg.chan=channel;
        cfg.atten=ADC_ATTEN_DB_12;cfg.bitwidth=ADC_BITWIDTH_12;
        err=adc_cali_create_scheme_curve_fitting(&cfg,&calibration);
    }
#else
    err=ESP_ERR_NOT_SUPPORTED;
#endif
    initialized=err==ESP_OK;
    ESP_LOGI("roast-battery","GPIO10 ADC init=%s calibration=%d board=%d",esp_err_to_name(err),initialized,(int)M5.getBoard());
}

int Hal::batteryVoltage()
{
    if(!initialized) return -1;
    int readings[9];
    for(int i=0;i<9;i++){
        int raw=0,millivolts=0;
        esp_err_t err=adc_oneshot_read(adc,channel,&raw);
        if(err==ESP_OK)err=adc_cali_raw_to_voltage(calibration,raw,&millivolts);
        if(err!=ESP_OK){ESP_LOGW("roast-battery","ADC read: %s",esp_err_to_name(err));return -1;}
        readings[i]=millivolts*2; // ADV's battery divider on GPIO10 is 2:1.
    }
    std::sort(readings,readings+9);
    return readings[4];
}
