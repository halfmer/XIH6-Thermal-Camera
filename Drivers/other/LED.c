#include "LED.h"
#include "DELAY.h"
#include "main.h"

void LED_SIGN_SET(void)
{
    HAL_GPIO_WritePin(LED_GPIO_GPIO_Port,LED_GPIO_Pin,GPIO_PIN_RESET);
}

void LED_SIGN_RESET(void)
{
    HAL_GPIO_WritePin(LED_GPIO_GPIO_Port,LED_GPIO_Pin,GPIO_PIN_SET);
}

void LED_TURN(unsigned int time)
{
    LED_SIGN_SET();
    delay_ms(time);
    LED_SIGN_RESET();
    delay_ms(time);
}
