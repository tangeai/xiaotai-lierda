#ifndef LITE_DEV_PLAYBACK_GAIN_HOST_GPIO_H
#define LITE_DEV_PLAYBACK_GAIN_HOST_GPIO_H
#include <stdbool.h>
typedef enum { L_GPIO_25 = 25 } liot_gpio_e;
typedef enum { L_IO_INPUT, L_IO_OUTPUT } liot_gpiodir_e;
typedef enum { L_IO_LOW, L_IO_HIGH, L_IO_NONE } liot_gpiolvl_e;
typedef enum { L_DOMAIN_NORMAL, L_DOMAIN_AON, L_DOMAIN_ALL } liot_powerdomain_e;
typedef enum { L_VOLT_3_30V = 21 } liot_volt_e;
typedef enum { L_GPIO_ERR_SUCCESS = 0 } liot_gpioerr_e;
typedef void (*liot_intcb_t)(void *);
liot_gpioerr_e Liot_GpioInit(liot_gpio_e gpio, liot_gpiodir_e direction,
                           liot_gpiolvl_e level, liot_intcb_t *callback);
liot_gpioerr_e Liot_AonPowerCtl(bool enable);
liot_gpioerr_e Liot_SetVoltage(liot_powerdomain_e domain, liot_volt_e voltage);
#endif
