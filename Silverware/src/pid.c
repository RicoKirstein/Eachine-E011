/*
The MIT License (MIT)

Copyright (c) 2016 silverx

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


//#define RECTANGULAR_RULE_INTEGRAL
//#define MIDPOINT_RULE_INTEGRAL
#define SIMPSON_RULE_INTEGRAL


//#define NORMAL_DTERM
#define NEW_DTERM
//#define MAX_FLAT_LPF_DIFF_DTERM

//#define ANTI_WINDUP_DISABLE

#include <stdbool.h>
#include <stdlib.h>
#include "pid.h"
#include "util.h"
#include "config.h"
#include "led.h"
#include "defines.h"


// B-03 firmware with modified hardware.h for E011

//      	                  ROLL       PITCH     YAW
float pidkp[PIDNUMBER] = { 9.0e-2 , 9.0e-2 , 5e-1 }; 
float pidki[PIDNUMBER] = { 6.0e-1 , 6.0e-1 , 3e-1 };	
float pidkd[PIDNUMBER] = { 6.0e-1 , 6.0e-1 , 0e-1 };


// "setpoint weighting" 0.0 - 1.0 where 1.0 = normal pid
// #define ENABLE_SETPOINT_WEIGHTING
float b[3] = { 1.0 , 1.0 , 1.0};




// output limit			
const float outlimit[PIDNUMBER] = { 0.8 , 0.8 , 0.5 };

// limit of integral term (abs)
const float integrallimit[PIDNUMBER] = { 0.8 , 0.8 , 0.5 };




// non changable things below
float * pids_array[3] = {pidkp, pidki, pidkd};

int number_of_increments[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
int current_pid_axis = 0;
int current_pid_term = 0;
float * current_pid_term_pointer = pidkp;

float ierror[PIDNUMBER] = { 0 , 0 , 0};	

float pidoutput[PIDNUMBER];

extern float error[PIDNUMBER];
static float lasterror[PIDNUMBER];

extern float looptime;
extern float gyro[3];
extern int onground;

extern float looptime;


#ifdef NORMAL_DTERM
static float lastrate[PIDNUMBER];
#endif

#ifdef NEW_DTERM
static float lastratexx[PIDNUMBER][2];
#endif

#ifdef MAX_FLAT_LPF_DIFF_DTERM
static float lastratexx[PIDNUMBER][4];
#endif

#ifdef SIMPSON_RULE_INTEGRAL
static float lasterror2[PIDNUMBER];
#endif

float timefactor;

extern int ledcommand;

void pid_precalc()
{
	timefactor = 0.0032f / looptime;
}

// Cycle through P / I / D - The initial value is P
// The return value is the currently selected TERM (after setting the next one)
// 1: P
// 2: I
// 3: D
// The return value is used to blink the leds in main.c
int next_pid_term()
{
//	current_pid_axis = 0;
	
	switch (current_pid_term)
	{
		case 0:
			current_pid_term_pointer = pidki;
			current_pid_term = 1;
			break;
		case 1:
			current_pid_term_pointer = pidkd;
			current_pid_term = 2;
			break;
		case 2:
			current_pid_term_pointer = pidkp;
			current_pid_term = 0;
			break;
	}
	
	return current_pid_term + 1;
}

// Cycle through the axis - Initial is Roll
// Return value is the selected axis, after setting the next one.
// 1: Roll
// 2: Pitch
// 3: Yaw
// The return value is used to blink the leds in main.c
int next_pid_axis()
{
	const int size = 3;
	if (current_pid_axis == size - 1) {
		current_pid_axis = 0;
	}
	else {
		#ifdef COMBINE_PITCH_ROLL_PID_TUNING
		if (current_pid_axis <2 ) {
			// Skip axis == 1 which is roll, and go directly to 2 (Yaw)
			current_pid_axis = 2;
		}
		#else
		current_pid_axis++;
		#endif
	}
	
	return current_pid_axis + 1;
}

#define PID_GESTURES_MULTI 1.1f

int change_pid_value(int increase)
{
	float multiplier = 1.0f/(float)PID_GESTURES_MULTI;
	if (increase) {
		multiplier = (float)PID_GESTURES_MULTI;
		number_of_increments[current_pid_term][current_pid_axis]++;
		#ifdef COMBINE_PITCH_ROLL_PID_TUNING
		//if (current_pid_axis == 0) {
		//	number_of_increments[current_pid_term][current_pid_axis+1]++;	
		//}
		#endif
	}
	else {
		number_of_increments[current_pid_term][current_pid_axis]--;
		#ifdef COMBINE_PITCH_ROLL_PID_TUNING
		//if (current_pid_axis == 0) {
		//	number_of_increments[current_pid_term][current_pid_axis+1]--;	
		//}
		#endif
	}
	current_pid_term_pointer[current_pid_axis] = current_pid_term_pointer[current_pid_axis] * multiplier;
	#ifdef COMBINE_PITCH_ROLL_PID_TUNING
	if (current_pid_axis == 0) {
		current_pid_term_pointer[current_pid_axis+1] = current_pid_term_pointer[current_pid_axis+1] * multiplier;
	}
	#endif
	
	return abs(number_of_increments[current_pid_term][current_pid_axis]);
}

// Increase currently selected term, for the currently selected axis, (by functions above) by 10%
// The return value, is absolute number of times the specific term/axis was increased or decreased.  For example, if P for Roll was increased by 10% twice,
// And then reduced by 10% 3 times, the return value would be 1  -  The user has to rememeber he has eventually reduced the by 10% and not increased by 10%
// I guess this can be improved by using the red leds for increments and blue leds for decrements or something, or just rely on SilverVISE
int increase_pid()
{
	return change_pid_value(1);
}

// Same as increase_pid but... you guessed it... decrease!
int decrease_pid()
{
	return change_pid_value(0);
}

float pid(int x )
{ 

        if (onground) 
				{
                ierror[x] *= 0.98f;
				}
	
				int iwindup = 0;
				if (( pidoutput[x] == outlimit[x] )&& ( error[x] > 0) )
				{
					iwindup = 1;		
				}
				if (( pidoutput[x] == -outlimit[x])&& ( error[x] < 0) )
				{
					iwindup = 1;				
				}              
                #ifdef ANTI_WINDUP_DISABLE
                iwindup = 0;
                #endif
        if ( !iwindup)
				{
				#ifdef MIDPOINT_RULE_INTEGRAL
				 // trapezoidal rule instead of rectangular
         ierror[x] = ierror[x] + (error[x] + lasterror[x]) * 0.5f *  pidki[x] * looptime;
				 lasterror[x] = error[x];
				#endif
					
				#ifdef RECTANGULAR_RULE_INTEGRAL
				 ierror[x] = ierror[x] + error[x] *  pidki[x] * looptime;
				 lasterror[x] = error[x];					
				#endif
					
				#ifdef SIMPSON_RULE_INTEGRAL
					// assuming similar time intervals
				 ierror[x] = ierror[x] + 0.166666f* (lasterror2[x] + 4*lasterror[x] + error[x]) *  pidki[x] * looptime;	
					lasterror2[x] = lasterror[x];
					lasterror[x] = error[x];
					#endif
					
				}
				
				limitf( &ierror[x] , integrallimit[x] );
				
				
                #ifdef ENABLE_SETPOINT_WEIGHTING
				// P term
                pidoutput[x] = error[x] * ( b[x])* pidkp[x];				
				// b
                pidoutput[x] +=  - ( 1.0f - b[x])* pidkp[x] * gyro[x];
				#else
                // P term with b disabled
                pidoutput[x] = error[x] * pidkp[x];
                #endif
				
				// I term	
				pidoutput[x] += ierror[x];
			
				// D term		  

				#ifdef NORMAL_DTERM
					pidoutput[x] = pidoutput[x] - (gyro[x] - lastrate[x]) * pidkd[x] * timefactor  ;
					lastrate[x] = gyro[x];
				#endif

                #ifdef NEW_DTERM
                    pidoutput[x] = pidoutput[x] - ( ( 0.5f) *gyro[x] 
                                - (0.5f) * lastratexx[x][1] ) * pidkd[x] * timefactor  ;
                                    
                    lastratexx[x][1] = lastratexx[x][0];
                    lastratexx[x][0] = gyro[x];
               #endif
			
               #ifdef MAX_FLAT_LPF_DIFF_DTERM 
					pidoutput[x] = pidoutput[x] - ( + 0.125f *gyro[x] + 0.250f * lastratexx[x][0]
								- 0.250f * lastratexx[x][2] - ( 0.125f) * lastratexx[x][3]) * pidkd[x] * timefactor 						;
				
					lastratexx[x][3] = lastratexx[x][2];
					lastratexx[x][2] = lastratexx[x][1];
					lastratexx[x][1] = lastratexx[x][0];
					lastratexx[x][0] = gyro[x];
				#endif            

				  limitf(  &pidoutput[x] , outlimit[x]);



return pidoutput[x];		 		
}


