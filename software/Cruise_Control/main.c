
#include <stdio.h>
#include "system.h"
#include "includes.h"
#include "altera_avalon_pio_regs.h"
#include "sys/alt_irq.h"
#include "sys/alt_alarm.h"


#define DEBUG 1

#define HW_TIMER_PERIOD 100 /* 100ms */

/* Button Patterns */

#define GAS_PEDAL_FLAG      0x08
#define BRAKE_PEDAL_FLAG    0x04
#define CRUISE_CONTROL_FLAG 0x02
/* Switch Patterns */

#define TOP_GEAR_FLAG       0x00000002
#define ENGINE_FLAG         0x00000001

/* LED Patterns */
#define LEDR17          0x20000 //position 0 - 399
#define LEDR16          0x10000 //position 400 - 799
#define LEDR15          0x08000 //position 800 - 1199
#define LEDR14          0x04000 //position 1200 - 1599
#define LEDR13          0x02000 //position 1600 - 1999
#define LEDR12          0x01000 //position 2000 - 2399

#define LED_RED_0 0x00000001 // Engine
#define LED_RED_1 0x00000002 // Top Gear

#define LED_GREEN_0 0x0001 // Cruising
#define LED_GREEN_2 0x0004 // Cruise Control Button
#define LED_GREEN_4 0x0010 // Brake Pedal
#define LED_GREEN_6 0x0040 // Gas Pedal

/*
 * Definition of Tasks
 */

#define TASK_STACKSIZE 2048

OS_STK StartTask_Stack[TASK_STACKSIZE];
OS_STK ControlTask_Stack[TASK_STACKSIZE];
OS_STK VehicleTask_Stack[TASK_STACKSIZE];
OS_STK DetectionTask_Stack[TASK_STACKSIZE];
OS_STK WatchDogTask_Stack[TASK_STACKSIZE];
OS_STK ExtraLoadTask_Stack[TASK_STACKSIZE];
OS_STK ButtonIOTask_Stack[TASK_STACKSIZE];
OS_STK SwitchIOTask_Stack[TASK_STACKSIZE];

// Task Priorities

#define STARTTASK_PRIO     5
#define VEHICLETASK_PRIO  10
#define CONTROLTASK_PRIO  12
#define DETECTIONTASK_PRIO    14
#define WATCHDOGTASK_PRIO     6
#define EXTRALOADTASK_PRIO   13
#define BUTTONIOTASK_PRIO  8 //highest priority of Input task
#define SWITCHIOTASK_PRIO  9 //second highest priority of Input task

// Task Periods

#define CONTROL_PERIOD  300
#define VEHICLE_PERIOD  300
#define BUTTON_PERIOD   100 //these value gives nice enough responsivity for SW timer based period
#define SWITCH_PERIOD   100

/*
 * Definition of Kernel Objects
 */

// Mailboxes
OS_EVENT *Mbox_Throttle;
OS_EVENT *Mbox_Velocity;
OS_EVENT *Mbox_Detection;
OS_EVENT *Mbox_Cruise_Control;
OS_EVENT *Mbox_Gas_Pedal;
OS_EVENT *Mbox_Brake_Pedal;
OS_EVENT *Mbox_Engine;
OS_EVENT *Mbox_Gear;

// Semaphores
OS_EVENT *buttonSem;
OS_EVENT *switchSem;
OS_EVENT *Vehicle_Sem;
OS_EVENT *Control_Sem;
OS_EVENT *Detection_Sem;
OS_EVENT *WatchDog_Sem;
OS_EVENT *ExtraLoad_Sem;

// SW-Timer
OS_TMR *buttonTmr;
OS_TMR *switchTmr;

OS_TMR *Vehicle_Tmr;
OS_TMR *Control_Tmr;
OS_TMR *Detection_Tmr;
OS_TMR *WatchDog_Tmr;
OS_TMR *ExtraLoad_Tmr;

/*
 * Types
 */
enum active {on, off};  // on = 0 ; off = 1

enum active gas_pedal = off;
enum active brake_pedal = off;
enum active top_gear = off;
enum active engine = off;
enum active cruise_control = off; //refers to user input
enum active cruising = off; //refers to vehicle state
enum active extra_load = off; //refers to vehicle state

/*
 * Global variables
 */
int delay; // Delay of HW-timer
int ExtraLoad_Percentage = 0; // The percentage of processing time of the extraload task
INT16U led_green = 0; // Green LEDs
INT32U led_red = 0;   // Red LEDs
INT16U led_extraload = 0; // Red LEDs for extraload

void draw_green_leds ()
{
    if (cruising == on)
    led_green += LED_GREEN_0;

if (cruise_control == on)
led_green += LED_GREEN_2;

if (brake_pedal == on)
led_green += LED_GREEN_4;

if (gas_pedal == on)
led_green += LED_GREEN_6;

IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE, led_green);
led_green = 0; //after writing leds, set led to 0 for next time
}

void draw_red_leds ()
{
if (engine == on)
led_red += LED_RED_0;

if (top_gear == on)
led_red += LED_RED_1;

if (extra_load == on)
led_red += led_extraload;

IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_REDLED18_BASE, led_red);
led_red = 0;//after writing leds, set led to 0 for next time
}

int buttons_pressed(void)
{
  return ~IORD_ALTERA_AVALON_PIO_DATA(DE2_PIO_KEYS4_BASE);
}

int switches_pressed(void)
{
  return IORD_ALTERA_AVALON_PIO_DATA(DE2_PIO_TOGGLES18_BASE);
}

void simulate_overload(int delay)
{
//float num_ticks_per_second = (float)alt_ticks_per_second();
  alt_u32 start = alt_nticks();
  // Wait for the desired number of ticks
  while((alt_nticks() - start)< delay);
}

/*
 * ISR for HW Timer
 */
alt_u32 alarm_handler(void* context)
{
  OSTmrSignal(); /* Signals a 'tick' to the SW timers */

  return delay;
}

/*
 * Soft Timer Callback Functions
 */
void Vehicle_Callback()
{
OSSemPost(Vehicle_Sem);
}

void Control_Callback()
{
OSSemPost(Control_Sem);
}

void Detection_Callback()
{
OSSemPost(Detection_Sem);
}

void WatchDog_Callback()
{
OSSemPost(WatchDog_Sem);
}

void ExtraLoad_Callback()
{
OSSemPost(ExtraLoad_Sem);
}

void makeAvailableButtonTask ()
{
OSSemPost(buttonSem);
}

void makeAvailableSwitchTask ()
{
OSSemPost(switchSem);
}


static int b2sLUT[] = {0x40, //0
       0x79, //1
       0x24, //2
       0x30, //3
       0x19, //4
       0x12, //5
       0x02, //6
       0x78, //7
       0x00, //8
       0x18, //9
       0x3F, //-
};

/*
 * convert int to seven segment display format
 */
int int2seven(int inval){
  return b2sLUT[inval];
}

/*
 * output current velocity on the seven segement display
 */
void show_velocity_on_sevenseg(INT8S velocity){
  int tmp = velocity;
  int out;
  INT8U out_high = 0;
  INT8U out_low = 0;
  INT8U out_sign = 0;

  if(velocity < 0){
    out_sign = int2seven(10);
    tmp *= -1;
  }else{
    out_sign = int2seven(0);
  }

  out_high = int2seven(tmp / 10);            //tens digit
  out_low = int2seven(tmp - (tmp/10) * 10); //single digit

  out = int2seven(0) << 21 |
    out_sign << 14 |
    out_high << 7  |
    out_low;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_HEX_LOW28_BASE,out);
}

/*
 * shows the target velocity on the seven segment display (HEX5, HEX4)
 * when the cruise control is activated (0 otherwise)
 */
void show_target_velocity(INT8U target_vel)
{
  int tmp = target_vel;
  int out;
  INT8U out_high = 0;
  INT8U out_low = 0;

  out_high = int2seven(tmp / 10);
  out_low = int2seven(tmp - (tmp/10) * 10);

  out = int2seven(0) << 21 |
int2seven(0) << 14 |
    out_high << 7  |
    out_low;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_HEX_HIGH28_BASE,out);
}

/*
 * indicates the position of the vehicle on the track with the four leftmost red LEDs
 * LEDR17: [0m, 400m)
 * LEDR16: [400m, 800m)
 * LEDR15: [800m, 1200m)
 * LEDR14: [1200m, 1600m)
 * LEDR13: [1600m, 2000m)
 * LEDR12: [2000m, 2400m]
 */
void show_position(INT16U position)
{
if(position < 4000)
led_red += LEDR17;
else if(position < 8000)
led_red += LEDR16;
else if(position < 12000)
led_red += LEDR15;
else if(position < 16000)
led_red += LEDR14;
else if(position < 20000)
led_red += LEDR13;
else if(position < 24000)
led_red += LEDR12;
}

/*
 * The function 'adjust_position()' adjusts the position depending on the
 * acceleration and velocity.
 */
INT16U adjust_position(INT16U position, INT16S velocity,
       INT8S acceleration, INT16U time_interval)
{
  INT16S new_position = position + velocity * time_interval / 1000
    + acceleration / 2  * (time_interval / 1000) * (time_interval / 1000);

  if (new_position > 24000) { //Why 24000 instead of 2400?
    new_position -= 24000;
  } else if (new_position < 0){
    new_position += 24000;
  }

  show_position(new_position);
  return new_position;
}

/*
 * The function 'adjust_velocity()' adjusts the velocity depending on the
 * acceleration.
 */
INT16S adjust_velocity(INT16S velocity, INT8S acceleration,
       enum active brake_pedal, INT16U time_interval)
{
  INT16S new_velocity;
  INT8U brake_retardation = 200;

  if (brake_pedal == off)
    new_velocity = velocity  + (float) (acceleration * time_interval) / 1000.0;
  else {
    if (brake_retardation * time_interval / 1000 > velocity)
      new_velocity = 0;
    else
      new_velocity = velocity - brake_retardation * time_interval / 1000;
  }

  return new_velocity;
}

/*
 * The task 'VehicleTask' updates the current velocity of the vehicle
 */
void VehicleTask(void* pdata)
{
  INT8U err;
  void* msg;
  INT8U* throttle;
  INT8S acceleration;  /* Value between 40 and -20 (4.0 m/s^2 and -2.0 m/s^2) */
  INT8S retardation;   /* Value between 20 and -10 (2.0 m/s^2 and -1.0 m/s^2) */
  INT16U position = 0; /* Value between 0 and 20000 (0.0 m and 2000.0 m)  */
  INT16S velocity = 0; /* Value between -200 and 700 (-20.0 m/s amd 70.0 m/s) */
  INT16S wind_factor;   /* Value between -10 and 20 (-1.0 m/s^2 and 2.0 m/s^2) */

  printf("Vehicle task created!\n");

  while(1)
    {
  OSSemPend(Vehicle_Sem, 0, &err);
      err = OSMboxPost(Mbox_Velocity, (void *) &velocity);

      //OSTimeDlyHMSM(0,0,0,VEHICLE_PERIOD);

      /* Non-blocking read of mailbox:
- message in mailbox: update throttle
- no message:         use old throttle
      */
      msg = OSMboxPend(Mbox_Throttle, 1, &err);
      if (err == OS_NO_ERR)
throttle = (INT8U*) msg;

      /* Retardation : Factor of Terrain and Wind Resistance */
      if (velocity > 0)
wind_factor = velocity * velocity / 10000 + 1;
      else
wind_factor = (-1) * velocity * velocity / 10000 + 1;

      if (position < 4000)
retardation = wind_factor; // even ground
      else if (position < 8000)
retardation = wind_factor + 15; // traveling uphill
      else if (position < 12000)
retardation = wind_factor + 25; // traveling steep uphill
      else if (position < 16000)
retardation = wind_factor; // even ground
      else if (position < 20000)
retardation = wind_factor - 10; //traveling downhill
      else
retardation = wind_factor - 5 ; // traveling steep downhill

      acceleration = *throttle / 2 - retardation;
      position = adjust_position(position, velocity, acceleration, 300);
      velocity = adjust_velocity(velocity, acceleration, brake_pedal, 300);
      printf("Position: %dm\n", position / 10);
      printf("Velocity: %4.1fm/s\n", velocity /10.0);
      printf("Throttle: %dV\n", *throttle / 10);
      show_velocity_on_sevenseg((INT8S) (velocity / 10));
    }
}

/*
 *  'Event handlers' and utilities
 */
void deactivateCruiseControl ()
{
cruising = off;
}

void activateCruiseControl (INT16S* current_velocity,
          INT16S* target_velocity)
{
cruising = on;
*target_velocity = *current_velocity; //TODO: use of pointers here?
}

void handleGasPedal ()
{
INT8U err;
void* msg;

msg = OSMboxPend(Mbox_Gas_Pedal, 1, &err);
if (msg == on)
{
gas_pedal = on;
deactivateCruiseControl ();
}
else if (msg == off)
{
gas_pedal = off;
}
}

void handleBrakePedal ()
{
INT8U err;
void* msg;

msg = OSMboxPend(Mbox_Brake_Pedal, 1, &err);
if (msg == on)
{
brake_pedal = on;
deactivateCruiseControl ();
}
else
{
brake_pedal = off;
}
}

int allowedToActivateCruiseControl (INT16S* current_velocity)
{
return  *current_velocity > 200 &&
top_gear == on &&
brake_pedal == off &&
gas_pedal == off;
}



void handleCruiseControl (INT16S* current_velocity,
  INT16S* target_velocity)
{
INT8U err;
void* msg;

msg = OSMboxPend(Mbox_Cruise_Control, 1, &err);
if (msg == on)
{
cruise_control = on;
if (allowedToActivateCruiseControl (current_velocity))
{
activateCruiseControl (current_velocity, target_velocity);
}
}
else if (msg == off)
{
cruise_control = off;
}
}

void handleTopGear ()
{
INT8U err;
void* msg;

msg = OSMboxPend(Mbox_Gear, 1, &err);
if (msg == on)
{
top_gear = on;
}
else if (msg == off)
{
top_gear = off;
deactivateCruiseControl ();
}
}



void handleEngine (INT16S* current_velocity,
           INT16S* target_velocity,
           INT8U* throttle)
{
INT8U err;
void* msg;

msg = OSMboxPend(Mbox_Engine, 1, &err);
if (msg == on)
{
engine = on;
}
else if (msg == off && *current_velocity < 1 && *current_velocity > -1) //if velocity is very close to 0, shut engine
{
engine = off;
deactivateCruiseControl ();
}

//THROTTLE, setting based on control logic
if (engine == on)
{
if (cruising == off) //in absence of CC, use pedals. TODO: think about this real hard
{
if (gas_pedal == on)
*throttle = 80;
else if (gas_pedal == off)
*throttle = 0;
}
else if (cruising == on) //PID Controller better than bang controller?
{
if (*current_velocity < *target_velocity)
*throttle = 80;
else if (*current_velocity > *target_velocity)
*throttle = 0;
}
}
}


/*
 * The task 'ControlTask' is the main task of the application. It reacts
 * on sensors and generates responses.
 */

void ControlTask(void* pdata)
{
  INT8U err;
  INT8U throttle = 40; /* Value between 0 and 80, which is interpreted as between 0.0V and 8.0V */
  void* msg;
  INT16S* current_velocity;
  INT16S* target_velocity;

  printf("Control Task created!\n");

  while(1)
    {
      OSSemPend(Control_Sem, 0, &err);
      msg = OSMboxPend(Mbox_Velocity, 0, &err);
      current_velocity = (INT16S*) msg;
      //GAS PEDAL CONTROL
      handleGasPedal ();

      //BRAKE PEDAL CONTROL
  handleBrakePedal ();

  //GEAR CONTROL
      handleTopGear ();

      //CRUISE CONTROL CONTROL
      handleCruiseControl (current_velocity, target_velocity);

  //ENGINE CONTROL
      handleEngine (current_velocity, target_velocity, &throttle);
      err = OSMboxPost (Mbox_Throttle, (void *) &throttle); //Post pointer to throttle



  //DISPLAY STUFF
      draw_red_leds ();
      draw_green_leds ();
      if (cruising == on)
      show_target_velocity ((INT16S) *target_velocity / 10);
      else
      show_target_velocity (0);
      //err = OSMboxPost(Mbox_Throttle, (void *) &throttle);
    }
}

/*
 * The task 'DetectionTask' keeps send the "OK" signal to the task 'WatchDogTask'periodically.
 * If the 'WatchDogTask' fails to receive the "OK" signal in a specific interval, then it indicates the system
 * is overloaded.
*/

void DetectionTask(void* pdata)
{
  INT8U err;
  //void* msg;

  printf("Detection Task created!\n");
  char* ok_signal = "OK!";

  while(1)
    {
      OSSemPend(Detection_Sem, 0, &err);
      err = OSMboxPost(Mbox_Detection, (void *) &ok_signal);
    }
}

/*
 *  The task 'WatchDog' check the "OK" signal from DetectionTask periodically.
 *  If it can not retrieve the "OK signal from DetectionTask in a specific interval,
 *  then it indicates the system is overloaded.
 */
void WatchDogTask(void* Data)
{
INT8U err;
//void* pmsg;
printf("WatchDog Task created!\n");

while(1)
{
OSSemPend(WatchDog_Sem, 0, &err);
OSMboxPend(Mbox_Detection, CONTROL_PERIOD, &err); // Check mailbox
//pmsg = OSMboxAccept(Mbox_Detection);
if ( err == OS_ERR_TIMEOUT)
//if ( pmsg == (void *)0)
{
printf(" System is overloaded! \n");
printf(" The workload of the original system is %d percents. \n ", 100-ExtraLoad_Percentage);
}
else
printf(" System is OK! \n ");
}

}

/*
 * The task 'ExtraLoad' impose an extra load to the system. By imposing extra load, it
 * may trigger a watchdog warning message. And we can learn about the load of the original system
 */
void ExtraLoadTask(void* Data)
{
INT8U err;
printf("ExtraLoad Task created! \n");
int switches_input, workload;
while(1)
{
OSSemPend(ExtraLoad_Sem, 0, &err);
switches_input = (switches_pressed() & 0x3f0) ;   //get the input value of SW9-SW4
//if(switches_input > 0)
//{
extra_load = on;
led_extraload = switches_input;
//}

workload = switches_input >> 3;
if(workload >= 100)
{
ExtraLoad_Percentage = 100;
simulate_overload(CONTROL_PERIOD);
//OSTimeDlyHMSM(0,0,0,CONTROL_PERIOD);
}
else
{
ExtraLoad_Percentage = workload;
simulate_overload(CONTROL_PERIOD*(workload)/100);
//OSTimeDlyHMSM(0,0,0,CONTROL_PERIOD*(workload)/100);
}
}
}

void ButtonIO (void* pdata)
{
  INT8U err;

while(1) {
/*Read buttons pressed*/
int keyPressed = buttons_pressed();


//READ BRAKE
if ((keyPressed & BRAKE_PEDAL_FLAG) != 0)
err = OSMboxPost(Mbox_Brake_Pedal, (void*) on);
else
err = OSMboxPost(Mbox_Brake_Pedal, (void*) off);


//READ GAS
if ((keyPressed & GAS_PEDAL_FLAG) != 0)
err = OSMboxPost(Mbox_Gas_Pedal, (void*) on);
else
err = OSMboxPost(Mbox_Gas_Pedal, (void*) off);


//READ CRUISE CONTROL
if ((keyPressed & CRUISE_CONTROL_FLAG) != 0)
err = OSMboxPost(Mbox_Cruise_Control, (void*) on);
else
err = OSMboxPost(Mbox_Cruise_Control, (void*) off);

OSSemPend(buttonSem, 0, &err); //Wait
}
}


void SwitchIO (void* pdata)
{
INT8U err;

while(1) {
int switchPressed = switches_pressed();


//READ GEAR
if ((switchPressed & TOP_GEAR_FLAG) != 0)
err = OSMboxPost(Mbox_Gear, (void*) on);
else
err = OSMboxPost(Mbox_Gear, (void*) off);


//READ ENGINE
if ((switchPressed & ENGINE_FLAG) != 0)
err = OSMboxPost(Mbox_Engine, (void*) on);
else
err = OSMboxPost(Mbox_Engine, (void*) off);


OSSemPend(switchSem, 0, &err);
}
}

/*
 * The task 'StartTask' creates all other tasks kernel objects and
 * deletes itself afterwards.
 */

void StartTask(void* pdata)
{
  INT8U err;
  void* context;

  static alt_alarm alarm;     /* Is needed for timer ISR function */

  /* Base resolution for SW timer : HW_TIMER_PERIOD ms */
  delay = alt_ticks_per_second() * HW_TIMER_PERIOD / 1000;
  printf("delay in ticks %d\n", delay);

  /*
   * Create Semaphores
   */
  Vehicle_Sem = OSSemCreate(0);
  Control_Sem = OSSemCreate(0);
  Detection_Sem = OSSemCreate(0);
  WatchDog_Sem = OSSemCreate(0);
  ExtraLoad_Sem = OSSemCreate(0);
  buttonSem = OSSemCreate(0);
  switchSem = OSSemCreate(0);

  /*
   * Create Hardware Timer with a period of 'delay'
   */
  if (alt_alarm_start (&alarm,
       delay,
       alarm_handler,
       context) < 0)
    {
      printf("No system clock available!n");
    }

  /*
   * Create and start Software Timer
   */
  WatchDog_Tmr = OSTmrCreate( 0, (CONTROL_PERIOD/100), OS_TMR_OPT_PERIODIC, WatchDog_Callback, NULL, "WatchDog_Task", &err);
  if (err == OS_ERR_NONE) { printf("WatchDog Soft Timer created! \n"); }
  OSTmrStart(WatchDog_Tmr, &err);
  if (err == OS_ERR_NONE) { printf("WatchDog Soft Timer started! \n"); }

  Vehicle_Tmr = OSTmrCreate( 0, (VEHICLE_PERIOD/100), OS_TMR_OPT_PERIODIC, Vehicle_Callback, NULL, "Vehicle Task", &err);
  if (err == OS_ERR_NONE) { printf("Vehicle Soft Timer created! \n"); }
  OSTmrStart(Vehicle_Tmr, &err);
  if (err == OS_ERR_NONE) { printf("Vehicle Soft Timer started! \n"); }

  Control_Tmr = OSTmrCreate( 0, (CONTROL_PERIOD/100), OS_TMR_OPT_PERIODIC, Control_Callback, NULL, "Control_Task", &err);
  if (err == OS_ERR_NONE) { printf("Control Soft Timer created! \n"); }
  OSTmrStart(Control_Tmr, &err);
  if (err == OS_ERR_NONE) { printf("Control Soft Timer started! \n"); }



  buttonTmr = OSTmrCreate( 0, (BUTTON_PERIOD/100), OS_TMR_OPT_PERIODIC, makeAvailableButtonTask, NULL, "button timer", &err);
  if (err == OS_ERR_NONE) { printf("button timer created! \n"); }
  OSTmrStart(buttonTmr, &err);
  if (err == OS_ERR_NONE) { printf("button timer started! \n"); }

  switchTmr = OSTmrCreate( 0, (SWITCH_PERIOD/100), OS_TMR_OPT_PERIODIC, makeAvailableSwitchTask, NULL, "switch timer", &err);
  if (err == OS_ERR_NONE) { printf("switch timer created! \n"); }
  OSTmrStart( switchTmr, &err);
  if (err == OS_ERR_NONE) { printf("switch timer started! \n"); }

  ExtraLoad_Tmr = OSTmrCreate( 0, (CONTROL_PERIOD/100), OS_TMR_OPT_PERIODIC, ExtraLoad_Callback, NULL, "ExtraLoad_Task", &err);
  if (err == OS_ERR_NONE) { printf("ExtraLoad Soft Timer created! \n"); }
  OSTmrStart(ExtraLoad_Tmr, &err);
  if (err == OS_ERR_NONE) { printf("ExtraLoad Soft Timer started! \n"); }

  Detection_Tmr = OSTmrCreate( 0, (CONTROL_PERIOD/100), OS_TMR_OPT_PERIODIC, Detection_Callback, NULL, "Detection_Task", &err);
  if (err == OS_ERR_NONE) { printf("Detection Soft Timer created! \n"); }
  OSTmrStart(Detection_Tmr, &err);
  if (err == OS_ERR_NONE) { printf("Detection Soft Timer started! \n"); }











  /*
   * Creation of Kernel Objects
   */

  // Mailboxes
  Mbox_Throttle =   OSMboxCreate((void*) 0); /* Empty Mailbox - Throttle */
  Mbox_Velocity =   OSMboxCreate((void*) 0); /* Empty Mailbox - Velocity */
  Mbox_Detection =   OSMboxCreate((void*) 0); /* Empty Mailbox - Detection */
  Mbox_Cruise_Control = OSMboxCreate ((void*) 0);
  Mbox_Gas_Pedal = OSMboxCreate ((void*) 0);
  Mbox_Brake_Pedal = OSMboxCreate ((void*) 0);
  Mbox_Engine = OSMboxCreate ((void*) 0);
  Mbox_Gear = OSMboxCreate ((void*) 0);

  /*
   * Create statistics task
   */

  OSStatInit();

  /*
   * Creating Tasks in the system
   */

  err = OSTaskCreateExt(
    ButtonIO, // Pointer to task code
  NULL,        // Pointer to argument that is
  // passed to task
  &ButtonIOTask_Stack[TASK_STACKSIZE-1], // Pointer to top
  // of task stack
  BUTTONIOTASK_PRIO,
  BUTTONIOTASK_PRIO,
  (void *)&ButtonIOTask_Stack[0],
  TASK_STACKSIZE,
  (void *) 0,
  OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
    SwitchIO, // Pointer to task code
  NULL,        // Pointer to argument that is
  // passed to task
  &SwitchIOTask_Stack[TASK_STACKSIZE-1], // Pointer to top
  // of task stack
  SWITCHIOTASK_PRIO,
  SWITCHIOTASK_PRIO,
  (void *)&SwitchIOTask_Stack[0],
  TASK_STACKSIZE,
  (void *) 0,
  OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
ControlTask, // Pointer to task code
NULL,        // Pointer to argument that is
                // passed to task
&ControlTask_Stack[TASK_STACKSIZE-1], // Pointer to top
// of task stack
CONTROLTASK_PRIO,
CONTROLTASK_PRIO,
(void *)&ControlTask_Stack[0],
TASK_STACKSIZE,
(void *) 0,
OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
VehicleTask, // Pointer to task code
NULL,        // Pointer to argument that is
                // passed to task
&VehicleTask_Stack[TASK_STACKSIZE-1], // Pointer to top
// of task stack
VEHICLETASK_PRIO,
VEHICLETASK_PRIO,
(void *)&VehicleTask_Stack[0],
TASK_STACKSIZE,
(void *) 0,
OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
ExtraLoadTask, // Pointer to task code
NULL,        // Pointer to argument that is
                // passed to task
&ExtraLoadTask_Stack[TASK_STACKSIZE-1], // Pointer to top
// of task stack
EXTRALOADTASK_PRIO,
EXTRALOADTASK_PRIO,
(void *)&ExtraLoadTask_Stack[0],
TASK_STACKSIZE,
(void *) 0,
OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
  WatchDogTask, // Pointer to task code
NULL,        // Pointer to argument that is
                // passed to task
&WatchDogTask_Stack[TASK_STACKSIZE-1], // Pointer to top
// of task stack
WATCHDOGTASK_PRIO,
WATCHDOGTASK_PRIO,
(void *)&WatchDogTask_Stack[0],
TASK_STACKSIZE,
(void *) 0,
OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
DetectionTask, // Pointer to task code
NULL,        // Pointer to argument that is
                // passed to task
&DetectionTask_Stack[TASK_STACKSIZE-1], // Pointer to top
// of task stack
DETECTIONTASK_PRIO,
DETECTIONTASK_PRIO,
(void *)&DetectionTask_Stack[0],
TASK_STACKSIZE,
(void *) 0,
OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
    ButtonIO, // Pointer to task code
  NULL,        // Pointer to argument that is
  // passed to task
  &ButtonIOTask_Stack[TASK_STACKSIZE-1], // Pointer to top
  // of task stack
  BUTTONIOTASK_PRIO,
  BUTTONIOTASK_PRIO,
  (void *)&ButtonIOTask_Stack[0],
  TASK_STACKSIZE,
  (void *) 0,
  OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
    SwitchIO, // Pointer to task code
  NULL,        // Pointer to argument that is
  // passed to task
  &SwitchIOTask_Stack[TASK_STACKSIZE-1], // Pointer to top
  // of task stack
  SWITCHIOTASK_PRIO,
  SWITCHIOTASK_PRIO,
  (void *)&SwitchIOTask_Stack[0],
  TASK_STACKSIZE,
  (void *) 0,
  OS_TASK_OPT_STK_CHK);




  printf("All Tasks and Kernel Objects generated!\n");

  /* Task deletes itself */

  OSTaskDel(OS_PRIO_SELF);
}

/*
 *
 * The function 'main' creates only a single task 'StartTask' and starts
 * the OS. All other tasks are started from the task 'StartTask'.
 *
 */

int main(void) {

  printf("Lab: Cruise Control\n");
  printf("Alt_ticks_per_second: %d. \n",alt_ticks_per_second());
  OSTaskCreateExt(
  StartTask, // Pointer to task code
  NULL,      // Pointer to argument that is
  // passed to task
  (void *)&StartTask_Stack[TASK_STACKSIZE-1], // Pointer to top
  // of task stack
  STARTTASK_PRIO,
  STARTTASK_PRIO,
  (void *)&StartTask_Stack[0],
  TASK_STACKSIZE,
  (void *) 0,
  OS_TASK_OPT_STK_CHK | OS_TASK_OPT_STK_CLR);

  OSStart();

  return 0;
}




