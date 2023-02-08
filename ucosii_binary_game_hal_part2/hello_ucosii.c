#include "altera_up_avalon_character_lcd.h"
#include "altera_avalon_pio_regs.h"
#include "includes.h"
#include "globals.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "time.h"

// MY DEVICE DRIVER PTRs
int alt_up_character_lcd_send_cmd();
struct alt_up_dev up_dev; // holds a pointer to each open I/O device
alt_up_character_lcd_dev * LCD_PTR;

// MY MUTEXs
OS_EVENT * StateMutex;		// Used for resource guarding (priority 2) - global variable "state"
OS_EVENT * TimerMutex;		// Used for resource guarding (priority 3) - global variables "elapsed_timer_value", "countdown_timer_value", "minutes", and "seconds"
OS_EVENT * GameMutex;		// Used for resource guarding (priority 4) - global variables "question_count", "current_score", and "attempt_count"

// MY FLAGs
OS_FLAG_GRP * stateFlag;	// Used for Pushbuttons - so, make respective event register bit high when a particular key is pressed

// MY MSG Qs
#define SIZE 16
void * Msg1Storage[SIZE];	// Used for declaring storage space for the Message Queues - with a SIZE (16)
OS_EVENT * MsgQ;			// Used for sending 3 random numbers generated in game_task() - received in the display_task() for displaying on the LCD

// MY SEMAPHOREs
OS_EVENT * SemRandom; 		// Used for signal-and-wait for the game_task() - so, only run game_task() when this semaphore posted
OS_EVENT * SemPower; 		// Used for signal-and-wait for the Power Switch - so, post the semaphore if the SW17 is high (ON)

/* Definition of Task Stacks */
#define TASK_STACKSIZE       			2048
OS_STK  power_switch_task_stk[TASK_STACKSIZE];
OS_STK  keys_task_stk[TASK_STACKSIZE];
OS_STK  timer_counter_task_stk[TASK_STACKSIZE];
OS_STK  game_task_stk[TASK_STACKSIZE];
OS_STK  lcdhex_display_task_stk[TASK_STACKSIZE];

/* Definition of Task Priorities */
#define power_switch_task_PRIORITY		1	// Checks if the power switch (SW17) is ON or OFF. Controls power for the whole system
#define keys_task_PRIORITY 				5	// Checking the event register and then based on that do things related to each key
#define timer_counter_task_PRIORITY 	6	// Runs evry 8 ticks, i.e., every 1 second so that the countdown timer and elapsed timer work as expected
#define game_task_PRIORITY 				7	// Called by a signal-and-wait semaphore - generates random numbers/operators and calculates system answer
#define lcdhex_display_task_PRIORITY 	8	// Displays on the HEXs and the LCD

// MY GLOBAL DEFs
#define IDLE    0
#define PLAY    1

// GLOBAL VARS
volatile int attempt_count = 0, question_count = 0, user_answer = 17, system_answer = 0, current_score = 0; 	// Guarded by GameMutex
volatile int seconds = 0, minutes = 0, elapsed_timer_value = 0, countdown_timer_value = 10; 					// Guarded by TimerMutex
volatile int ones_place = 0, tens_place = 0, huns_place = 0, thos_place = 0; 									// Guarded by <nothing>
volatile int state = IDLE; 																						// Guarded by StateMutex

// MY GLOBAL FNCs
int  DEC_TO_HEX(int IN_value)
{
    int converted_HEX = 0;

    switch (IN_value)
    {
        case 0x0: converted_HEX = 0x3F; break;
        case 0x1: converted_HEX = 0x06; break;
        case 0x2: converted_HEX = 0x5B; break;
        case 0x3: converted_HEX = 0x4F; break;
        case 0x4: converted_HEX = 0x66; break;
        case 0x5: converted_HEX = 0x6D; break;
        case 0x6: converted_HEX = 0x7D; break;
        case 0x7: converted_HEX = 0x07; break;
        case 0x8: converted_HEX = 0x7F; break;
        case 0x9: converted_HEX = 0x6F; break;
        default: break;
    }

    return converted_HEX;
}
void POWER_OFF_RESET()
{
	INT8U err;

	ones_place = 0;
    tens_place = 0;
    huns_place = 0;
    thos_place = 0;

    // State
	OSMutexPend(StateMutex, 0, &err);
	state = IDLE;
	OSMutexPost(StateMutex);

	// Timer Values
	OSMutexPend(TimerMutex, 0, &err);
	elapsed_timer_value = 0;
	countdown_timer_value = 10;
	seconds = 0;
	minutes = 0;
	OSMutexPost(TimerMutex);

	// Game Variables
	OSMutexPend(GameMutex, 0, &err);
	current_score = 0;
	attempt_count = 0;
	question_count = 0;
	OSMutexPost(GameMutex);

	// Calling Game Task
    OSSemPost(SemRandom);
}

// MY TASKs
void power_switch_task(void * pdata)
{
	int SW_value = 0;

	while (1)
    {
    	SW_value = IORD_ALTERA_AVALON_PIO_DATA(SLIDER_SWITCHES_BASE);

        if ((SW_value & 0x20000) == 131072) { OSSemPost(SemPower); }
        else { POWER_OFF_RESET(); }

		OSTimeDly(1);
    }
}

void pushbutton_task(struct alt_up_dev * up_dev, unsigned int id)
{
	int KEY_value = 0;

	INT8U err;

	KEY_value = IORD_ALTERA_AVALON_PIO_EDGE_CAP(PUSHBUTTONS_BASE);
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSHBUTTONS_BASE,KEY_value); // clear the interrupt - very important

	if ((KEY_value == 0b001) && (state != PLAY))
	{
		OSFlagPost(stateFlag, 0x01, OS_FLAG_SET, &err);
	}

	if (KEY_value == 0b010)
	{
		OSFlagPost(stateFlag, 0x02, OS_FLAG_SET, &err);
		OSMutexPost(StateMutex);
	}

	if ((KEY_value == 0b100) && (state != IDLE))
	{
		OSFlagPost(stateFlag, 0x04, OS_FLAG_SET, &err);
	}
}

void keys_task(void * pdata) // state machine task
{
	OS_FLAGS value;
	INT8U err;

	int user_answer;

	while (1)
	{
		value = OSFlagAccept(stateFlag, 0x04 + 0x02 + 0x01, OS_FLAG_WAIT_SET_ANY + OS_FLAG_CONSUME, &err);

		if ((value & 0x01) == 0x01)
		{
			POWER_OFF_RESET();
		}

		if ((value & 0x02) == 0x02)
		{
			if (question_count < 11)
			{
				OSMutexPend(StateMutex, 0, &err);
				if (state == IDLE) { state = PLAY; }
				else if (state == PLAY) { state = IDLE; }
				OSMutexPost(StateMutex);
			}
		}

		if ((value & 0x04) == 0x04)
		{
			OSMutexPend(GameMutex, 0, &err);
			attempt_count += 1;
			user_answer = IORD_ALTERA_AVALON_PIO_DATA(SLIDER_SWITCHES_BASE) & 0xF;

			// CHECK ANSWER
			if ((user_answer == system_answer) && (attempt_count == 1))
			{
				current_score += 1;
				attempt_count = 0;

				OSMutexPend(TimerMutex, 0, &err);
				countdown_timer_value = 10;
				OSMutexPost(TimerMutex);

				user_answer = ~user_answer;
			    OSSemPost(SemRandom);
			}
			else if ((user_answer != system_answer) && (attempt_count == 1))
			{
				attempt_count = 0;

				OSMutexPend(TimerMutex, 0, &err);
				countdown_timer_value = 10;
				OSMutexPost(TimerMutex);

				user_answer = ~user_answer;
			    OSSemPost(SemRandom);
			}

			OSMutexPost(GameMutex);
		}

		printf("\nQuestion: %d", question_count);
		printf("\n%d", system_answer);

		OSTimeDly(2);
	}
}

void timer_counter_task(void * pdata)
{
	INT8U err;

	while (1)
	{
		if (state == PLAY)
		{
			OSMutexPend(TimerMutex, 0, &err);
		    countdown_timer_value -= 1;
		    if (countdown_timer_value == 0)
		    {
		        countdown_timer_value = 10;
		        OSSemPost(SemRandom);
		    }

		    elapsed_timer_value += 1;
		    seconds += 1;
		    if ((elapsed_timer_value%60) == 0 && elapsed_timer_value != 0)
		    {
		        minutes += 1;
		        seconds = 0;
		    }

			OSMutexPost(TimerMutex);
		}

		OSTimeDly(8);
	}
}

void game_task(void * pdata)
{
	INT8U err;

	INT16U random_1;
	INT16U random_2;
	INT16U random_3;

    while (1)
    {
    	srand(time(0));

		OSSemPend(SemRandom, 0, &err);
		random_1 = rand() % 16;
		random_2 = rand() % 16;
		random_3 = rand() % 6;

    	OSQPost(MsgQ , (void *)&random_1);
    	OSQPost(MsgQ , (void *)&random_2);
    	OSQPost(MsgQ , (void *)&random_3);

		if (state == PLAY)
		{
			if      (random_3 == 0) { system_answer = abs(random_1 & random_2);    }
			else if (random_3 == 1) { system_answer = abs(~(random_1 & random_2)); }
			else if (random_3 == 2) { system_answer = abs(random_1 | random_2);    }
			else if (random_3 == 3) { system_answer = abs(~(random_1 | random_2)); }
			else if (random_3 == 4) { system_answer = abs(random_1 ^ random_2);    }
			else if (random_3 == 5) { system_answer = abs(~(random_1 ^ random_2)); }

			if (system_answer == 16) { system_answer = 0; } // 16 is basically 4-bit zero with a carry forwarded 1
		}

		OSMutexPend(GameMutex, 0, &err);
		question_count += 1;
		OSMutexPost(GameMutex);

		OSTimeDly(1);
    }
}

void lcdhex_display_task(void * pdata)
{
	char * numbers[16] = { "0000", "0001", "0010", "0011", "0100", "0101", "0110", "0111", "1000", "1001", "1010", "1011", "1100", "1101", "1110", "1111" };
	char * operator[6] = { "AND ", "NAND", "OR  ", "NOR ", "XOR ", "XNOR" };

	char * random_number_1 = "";
	char * random_number_2 = "";
	char * random_operator = "";

	INT8U err;

	INT16U *msg1;
	INT16U *msg2;
	INT16U *msg3;

	INT16U random_1_from_msgq;
	INT16U random_2_from_msgq;
	INT16U random_3_from_msgq;

	while (1)
	{
		if (OSSemAccept(SemPower) > 0)
		{
			// CURRENT SCORE DISPLAY (HEX)
			OSMutexPend(GameMutex, 0, &err);
			huns_place = DEC_TO_HEX(current_score%10);
			thos_place = DEC_TO_HEX((current_score/10)%10);
			OSMutexPost(GameMutex);

			// COUNTDOWN TIMER DISPLAY (HEX)
			OSMutexPend(TimerMutex, 0, &err);

			if ((question_count == 11) || (current_score == 10))
			{
				ones_place = 0x0000000;
				tens_place = 0x0000000;
			}
			else
			{
				ones_place = DEC_TO_HEX(countdown_timer_value%10);
				tens_place = DEC_TO_HEX((countdown_timer_value/10)%10);
			}

			IOWR_ALTERA_AVALON_PIO_DATA(HEX7_HEX4_BASE, ones_place | (tens_place << 8) | (huns_place << 16) | (thos_place << 24));

			// ELAPSED TIMER DISPLAY (HEX)
			ones_place = DEC_TO_HEX(seconds%10);
			tens_place = DEC_TO_HEX((seconds/10)%10);
			huns_place = DEC_TO_HEX(minutes%10);
			thos_place = DEC_TO_HEX((minutes/10)%10);

			IOWR_ALTERA_AVALON_PIO_DATA(HEX3_HEX0_BASE, ones_place | (tens_place << 8) | (huns_place << 16) | (thos_place << 24));

			OSMutexPost(TimerMutex);

			// QUESTION DISPLAY (LCD)
			if ((state == IDLE) || ((question_count == 11) || (current_score == 10))) // Hide the question when in PAUSE state
			{
	  			alt_up_character_lcd_send_cmd(LCD_PTR, ALT_UP_CHARACTER_LCD_COMM_CLEAR_DISPLAY);

				OSMutexPend(StateMutex, 0, &err);
				state = IDLE;
				OSMutexPost(StateMutex);
			}
			else
			{
				if (question_count < 11)
				{
					msg1 = OSQAccept(MsgQ,&err);
					msg2 = OSQAccept(MsgQ,&err);
					msg3 = OSQAccept(MsgQ,&err);

					if (err == OS_ERR_NONE)
					{
						random_1_from_msgq = *msg1;
						random_2_from_msgq = *msg2;
						random_3_from_msgq = *msg3;
					}

					random_number_1 = numbers[random_1_from_msgq];
					random_number_2 = numbers[random_2_from_msgq];
					random_operator = operator[random_3_from_msgq];

					// Number 1: LCD 3-0
					alt_up_character_lcd_set_cursor_pos(LCD_PTR, 0, 0);
					alt_up_character_lcd_string(LCD_PTR, random_number_1);

					// Number 2: LCD 2nd row 43-40
					alt_up_character_lcd_set_cursor_pos(LCD_PTR, 0, 1);
					alt_up_character_lcd_string(LCD_PTR, random_number_2);

					// Operator: LCD 2nd row 4F-4C
					alt_up_character_lcd_set_cursor_pos(LCD_PTR, 12, 1);
					alt_up_character_lcd_string(LCD_PTR, random_operator);
				}
			}
		}
		else
		{
			IOWR_ALTERA_AVALON_PIO_DATA(HEX7_HEX4_BASE, 0);
			IOWR_ALTERA_AVALON_PIO_DATA(HEX3_HEX0_BASE, 0);
  			alt_up_character_lcd_send_cmd(LCD_PTR, ALT_UP_CHARACTER_LCD_COMM_CLEAR_DISPLAY);
		}

		OSTimeDly(1);
	}
}

// MY MAIN
int main(void)
{
	OSTaskCreateExt
  	(
  		power_switch_task,
        NULL,
        (void *)&power_switch_task_stk[TASK_STACKSIZE-1],
		power_switch_task_PRIORITY,
		power_switch_task_PRIORITY,
		power_switch_task_stk,
        TASK_STACKSIZE,
        NULL,
        0
	);

	OSTaskCreateExt
  	(
  		keys_task,
        NULL,
        (void *)&keys_task_stk[TASK_STACKSIZE-1],
		keys_task_PRIORITY,
		keys_task_PRIORITY,
		keys_task_stk,
        TASK_STACKSIZE,
        NULL,
        0
	);

	OSTaskCreateExt
  	(
  		timer_counter_task,
        NULL,
        (void *)&timer_counter_task_stk[TASK_STACKSIZE-1],
		timer_counter_task_PRIORITY,
		timer_counter_task_PRIORITY,
		timer_counter_task_stk,
        TASK_STACKSIZE,
        NULL,
        0
	);

	OSTaskCreateExt
  	(
  		game_task,
        NULL,
        (void *)&game_task_stk[TASK_STACKSIZE-1],
		game_task_PRIORITY,
		game_task_PRIORITY,
		game_task_stk,
        TASK_STACKSIZE,
        NULL,
        0
	);

	OSTaskCreateExt
  	(
  		lcdhex_display_task,
        NULL,
        (void *)&lcdhex_display_task_stk[TASK_STACKSIZE-1],
		lcdhex_display_task_PRIORITY,
		lcdhex_display_task_PRIORITY,
		lcdhex_display_task_stk,
        TASK_STACKSIZE,
        NULL,
        0
	);

	INT8U err;

	// My Device Driver Ports

	// KEYs
	IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PUSHBUTTONS_BASE, 0xF);
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSHBUTTONS_BASE, 0xF);
	alt_irq_register(1, (void *)&up_dev, (void *)pushbutton_task);

	// LCD
	LCD_PTR = alt_up_character_lcd_open_dev("/dev/Char_LCD_16x2");
	if (LCD_PTR == NULL) { printf ("Error: could not open character LCD device\n"); }
	else { printf ("Opened character LCD device\n"); up_dev.LCD_PTR = LCD_PTR; } 	// store for use by ISRs
	alt_up_character_lcd_init(LCD_PTR); 											// Initialize character display

	// My Mutex Create - Here, it means the resource mutex's corresponding task will get highest priority of 2 or 3 or 4 (similar to HLP)
	StateMutex = OSMutexCreate(2, &err);
	TimerMutex = OSMutexCreate(3, &err);
	GameMutex = OSMutexCreate(4, &err);

	// My Event Flags
	stateFlag = OSFlagCreate(0x00, &err);

	// My Message Creates
	MsgQ = OSQCreate(&Msg1Storage[0], SIZE);

	// My Semaphore Creates
	SemRandom = OSSemCreate(1);
	SemPower = OSSemCreate(0);

	OSStart();
	return 0;
}
