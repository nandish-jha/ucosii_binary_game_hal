/* The base addresses of devices are listed in the BSP/system.h file*/
#include "system.h"

/* include HAL device driver functions for the parallel port and audio device */
#include "altera_avalon_pio_regs.h"
#include "altera_up_avalon_character_lcd.h"
#include "altera_up_avalon_character_lcd_regs.h"

#include "sys/alt_stdio.h"
#include "sys/alt_irq.h"

#ifndef LEDS_BASE
#define LEDS_BASE RED_LEDS_BASE
#endif

/* This stucture holds a pointer to each open I/O device */
struct alt_up_dev
{
	alt_up_character_lcd_dev * LCD_PTR;
};
