/*
 **
 ******************************************************************************
 * @file           : MESCfoc.c
 * @brief          : FOC running code and ADC buffers
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; Copyright (c) 2022 David Molony.
 * All rights reserved.</center></h2>
 *
 * This software component is licensed under BSD 3-Clause license,
 * the "License"; You may not use this file except in compliance with the
 * License. You may obtain a copy of the License at:
 *                        opensource.org/licenses/BSD-3-Clause
 ******************************************************************************
 *In addition to the usual 3 BSD clauses, it is explicitly noted that you
 *do NOT have the right to take sections of this code for other projects
 *without attribution and credit to the source. Specifically, if you copy into
 *copyleft licenced code without attribution and retention of the permissive BSD
 *3 clause licence, you grant a perpetual licence to do the same regarding turning sections of your code
 *permissive, and lose any rights to use of this code previously granted or assumed.
 *
 *This code is intended to remain permissively licensed wherever it goes,
 *maintaining the freedom to distribute compiled binaries WITHOUT a requirement to supply source.
 *
 *This is to ensure this code can at any point be used commercially, on products that may require
 *such restriction to meet regulatory requirements, or to avoid damage to hardware, or to ensure
 *warranties can reasonably be honoured.
 ******************************************************************************

 * MESCfoc.c
 *
 *  Created on: 18 Jul 2020
 *      Author: David Molony
 */

/* Includes ------------------------------------------------------------------*/
#include "MESCfoc.h"

#include "MESChw_setup.h"
#include "MESCmotor_state.h"
#include "MESCsin_lut.h"
#include "MESCmotor.h"
#include "MESCtemp.h"
#include "MESCspeed.h"
#include "MESCerror.h"

#include <math.h>
#include <stdlib.h>

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim4;
extern ADC_HandleTypeDef hadc1, hadc2, hadc3, hadc4;

float one_on_sqrt6 = 0.408248f;
float one_on_sqrt3 = 0.577350f;
float one_on_sqrt2 = 0.707107f;
float sqrt_two_on_3 = 0.816497f;
float sqrt3_2 = 1.22474f;
float sqrt2 = 1.41421f;
float sqrt1_2 = 0.707107f;
float sqrt3_on_2 = 0.866025f;
float two_on_sqrt3 = 1.73205f;
int adc_conv_end;

MESC_motor_typedef mtr[NUM_MOTORS];

MESCtest_s test_vals;
input_vars_t input_vars;
sampled_vars_t sampled_vars;

int print_samples_now, lognow;

//Debug
#define DEMCR_TRCENA    0x01000000
#define DEMCR           (*((volatile uint32_t *)0xE000EDFC))
#define DWT_CTRL        (*(volatile uint32_t *)0xe0001000)
#define CYCCNTENA       (1<<0)
#define DWT_CYCCNT      ((volatile uint32_t *)0xE0001004)
#define CPU_CYCLES      *DWT_CYCCNT


static void SlowHFI(MESC_motor_typedef *_motor);
static void calculatePower(MESC_motor_typedef *_motor);
static void LimitFWCurrent(MESC_motor_typedef *_motor);
static void houseKeeping(MESC_motor_typedef *_motor);
static void clampBatteryPower(MESC_motor_typedef *_motor);
static void ThrottleTemperature(MESC_motor_typedef *_motor);
static void FWRampDown(MESC_motor_typedef *_motor);

void MESCInit(MESC_motor_typedef *_motor) {
#ifdef STM32L4 // For some reason, ST have decided to have a different name for the L4 timer DBG freeze...
	DBGMCU->APB2FZ |= DBGMCU_APB2FZ_DBG_TIM1_STOP;
#else
	DBGMCU->APB2FZ |= DBGMCU_APB2_FZ_DBG_TIM1_STOP;
#endif
	_motor->MotorState = MOTOR_STATE_IDLE;

	//enable cycle counter
	DEMCR |= DEMCR_TRCENA;
	DWT_CTRL |= CYCCNTENA;
	_motor->m = *motor_profile;

	_motor->offset.Iu = ADC_OFFSET_DEFAULT;
	_motor->offset.Iv = ADC_OFFSET_DEFAULT;
	_motor->offset.Iw = ADC_OFFSET_DEFAULT;

	_motor->MotorState = MOTOR_STATE_INITIALISING;

	//At this stage, we initialise the options
	_motor->MotorControlType = MOTOR_CONTROL_TYPE_FOC;
	//_motor->MotorControlType = MOTOR_CONTROL_TYPE_BLDC;

	_motor->MotorSensorMode = DEFAULT_SENSOR_MODE;
	_motor->HFIType = DEFAULT_HFI_TYPE;

	_motor->meas.measure_current = I_MEASURE;
	_motor->meas.measure_voltage = V_MEASURE;
	_motor->FOC.pwm_frequency =PWM_FREQUENCY;
	_motor->meas.hfi_voltage = HFI_VOLTAGE;

	//Init Hall sensor
	_motor->hall.dir = 1.0f;
	_motor->hall.ticks_since_last_observer_change = 65535.0f;
	_motor->hall.last_observer_period = 65536.0f;
	_motor->hall.one_on_last_observer_period = 1.0f;
	_motor->hall.angular_velocity = 0;
	_motor->hall.angle_step = 0;

	_motor->hall.hall_error = 0;
//Init the BLDC
	_motor->BLDC.com_flux = _motor->m.flux_linkage*1.65f;//0.02f;
	_motor->BLDC.direction = -1;

	 mesc_init_1(_motor);

	HAL_Delay(3000);  // Give the everything else time to start up (e.g. throttle,
					// controller, PWM source...)

	mesc_init_2(_motor);

	hw_init(_motor);  // Populate the resistances, gains etc of the PCB - edit within
			  // this function if compiling for other PCBs
//Reconfigure dead times
//This is only useful up to 1500ns for 168MHz clock, 3us for an 84MHz clock
#ifdef CUSTOM_DEADTIME
  uint32_t tempDT;
  uint32_t tmpbdtr = 0U;
  tmpbdtr = mtr->mtimer->Instance->BDTR;
  tempDT = (uint32_t)(((float)CUSTOM_DEADTIME * (float)HAL_RCC_GetHCLKFreq())/(float)1000000000.0f);
  if(tempDT<128){
  MODIFY_REG(tmpbdtr, TIM_BDTR_DTG, tempDT);
  }else{
	  uint32_t deadtime = CUSTOM_DEADTIME;
	  deadtime = deadtime-(uint32_t)(127.0f*1000000000.0f/(float)HAL_RCC_GetHCLKFreq());
	  tempDT = 0b10000000 + (uint32_t)(((float)deadtime * (float)HAL_RCC_GetHCLKFreq())/(float)2000000000.0f);
	  MODIFY_REG(tmpbdtr, TIM_BDTR_DTG, tempDT);
  }
  mtr->mtimer->Instance->BDTR = tmpbdtr;
#endif

	// Start the PWM channels, reset the counter to zero each time to avoid
	// triggering the ADC, which in turn triggers the ISR routine and wrecks the
	// startup
	mesc_init_3(_motor);

	calculateGains(_motor);
	calculateVoltageGain(_motor);

#ifdef LOGGING
  lognow = 1;
#endif

#ifdef USE_ENCODER
  _motor->FOC.enc_offset = ENCODER_E_OFFSET;
#endif

	//Set up the input capture for throttle
	HAL_TIM_IC_Start(_motor->stimer, TIM_CHANNEL_1);
	HAL_TIM_IC_Start(_motor->stimer, TIM_CHANNEL_2);
	// Here we can auto set the prescaler to get the us input regardless of the main clock
	__HAL_TIM_SET_PRESCALER(_motor->stimer, (HAL_RCC_GetHCLKFreq() / 1000000 - 1));
	__HAL_TIM_SET_AUTORELOAD(_motor->stimer,1000000 / SLOW_LOOP_FREQUENCY); //Run slowloop at 100Hz
	__HAL_TIM_ENABLE_IT(_motor->stimer, TIM_IT_UPDATE);

  InputInit();

  	  //htim1.Instance->BDTR |=TIM_BDTR_MOE;
	  // initialising the comparators triggers the break state,
	  // so turn it back on
	  // At this point we just let the whole thing run off into interrupt land, and
	  // the fastLoop() starts to be triggered by the ADC conversion complete
	  // interrupt

  _motor->conf_is_valid = true;
}

void InputInit(){

	input_vars.max_request_Idq.d = 0.0f; //Not supporting d-axis input current for now
	input_vars.min_request_Idq.d = 0.0f;
	if(!input_vars.max_request_Idq.q){
	input_vars.max_request_Idq.q = MAX_IQ_REQUEST;
	input_vars.min_request_Idq.q = -MAX_IQ_REQUEST;
	}

	input_vars.IC_pulse_MAX = IC_PULSE_MAX;
	input_vars.IC_pulse_MIN = IC_PULSE_MIN;
	input_vars.IC_pulse_MID = IC_PULSE_MID;
	input_vars.IC_pulse_DEADZONE = IC_PULSE_DEADZONE;
	input_vars.IC_duration_MAX = IC_DURATION_MAX;
	input_vars.IC_duration_MIN = IC_DURATION_MIN;


	input_vars.adc1_MAX = ADC1MAX;
	input_vars.adc1_MIN = ADC1MIN;

	input_vars.adc2_MAX = ADC2MAX;
	input_vars.adc2_MIN = ADC2MIN;

	input_vars.adc1_gain[0] = (input_vars.max_request_Idq.d)/(input_vars.adc1_MAX-input_vars.adc1_MIN);
	input_vars.adc1_gain[1] = (input_vars.max_request_Idq.q)/(input_vars.adc1_MAX-input_vars.adc1_MIN);

	input_vars.adc2_gain[0] = (input_vars.max_request_Idq.d)/(input_vars.adc2_MAX-input_vars.adc2_MIN);
	input_vars.adc2_gain[1] = (input_vars.max_request_Idq.q)/(input_vars.adc2_MAX-input_vars.adc2_MIN);

	//RCPWM forward gain//index [0][x] is used for Idq requests for now, might support asymmetric brake and throttle later
	input_vars.RCPWM_gain[0][0] = (input_vars.max_request_Idq.d)/((float)input_vars.IC_pulse_MAX - (float)input_vars.IC_pulse_MID - (float)input_vars.IC_pulse_DEADZONE);
	input_vars.RCPWM_gain[0][1] = (input_vars.max_request_Idq.q)/(((float)input_vars.IC_pulse_MID - (float)input_vars.IC_pulse_DEADZONE)-(float)input_vars.IC_pulse_MIN);

	input_vars.input_options = DEFAULT_INPUT;
	input_vars.ADC1_polarity = ADC1_POLARITY;
	input_vars.ADC2_polarity = ADC2_POLARITY;

	input_vars.Idq_req_UART.d =0;
	input_vars.Idq_req_RCPWM.d =0;
	input_vars.Idq_req_ADC1.d =0;
	input_vars.Idq_req_ADC2.d =0;
	input_vars.Idq_req_UART.q =0;
	input_vars.Idq_req_RCPWM.q =0;
	input_vars.Idq_req_ADC1.q =0;
	input_vars.Idq_req_ADC2.q =0;

}
void initialiseInverter(MESC_motor_typedef *_motor){
static int Iuoff, Ivoff, Iwoff;
      Iuoff += _motor->Raw.Iu;
      Ivoff += _motor->Raw.Iv;
      Iwoff += _motor->Raw.Iw;

      static int initcycles = 0;
      initcycles = initcycles + 1;
      //Exit the initialisation after 1000cycles
      if (initcycles == 1000) {
        calculateGains(_motor);
        calculateVoltageGain(_motor);
        _motor->FOC.flux_b = 0.001f;
        _motor->FOC.flux_a = 0.001f;

        _motor->offset.Iu =  Iuoff/initcycles;
        _motor->offset.Iv =  Ivoff/initcycles;
        _motor->offset.Iw =  Iwoff/initcycles;
//ToDo, do we want some safety checks here like offsets being roughly correct?
    	_motor->MotorState = MOTOR_STATE_TRACKING;
        htim1.Instance->BDTR |= TIM_BDTR_MOE;
      }
}


// This should be the only function needed to be added into the PWM interrupt
// for MESC to run Ensure that it is followed by the clear timer update
// interrupt
void MESC_PWM_IRQ_handler(MESC_motor_typedef *_motor) {
  uint32_t cycles = CPU_CYCLES;
  if (_motor->mtimer->Instance->CNT > 512) {
    //_motor->FOC.IRQentry = debugtim.Instance->CNT;
    fastLoop(_motor);
    //_motor->FOC.IRQexit = debugtim.Instance->CNT - _motor->FOC.IRQentry;
    //_motor->FOC.FLrun++;
    _motor->FOC.cycles_fastloop = CPU_CYCLES - cycles;
  } else {
    //_motor->FOC.IRQentry = debugtim.Instance->CNT;
    hyperLoop(_motor);
    //_motor->FOC.IRQexit = debugtim.Instance->CNT - _motor->FOC.IRQentry;
    //_motor->FOC.VFLrun++;
    _motor->FOC.cycles_hyperloop = CPU_CYCLES - cycles;
  }
}

// The fastloop runs at PWM timer counter top, which is when the new ADC current
// readings arrive.
// The first few clock cycles of the interrupt should not use the adc readings,
// since the currents require approximately 1us = 144 clock cycles (f405) and 72
// clock cycles (f303) to convert.

void fastLoop(MESC_motor_typedef *_motor) {
  // Call this directly from the TIM top IRQ
  _motor->hall.current_hall_state = getHallState(); //ToDo, this macro is not applicable to dual motors
  // First thing we ever want to do is convert the ADC values
  // to real, useable numbers.
  ADCConversion(_motor);

  switch (_motor->MotorState) {

  	case MOTOR_STATE_INITIALISING:
  		initialiseInverter(_motor);
  		break;

    case MOTOR_STATE_RUN:
      if (_motor->MotorSensorMode == MOTOR_SENSOR_MODE_HALL) {
			_motor->FOC.inject = 0;
			hallAngleEstimator();
			angleObserver(_motor);
			MESCFOC(_motor);
			writePWM(_motor);
      } else if (_motor->MotorSensorMode == MOTOR_SENSOR_MODE_SENSORLESS) {
#ifdef USE_HALL_START
		  static int hall_start_now;
		if((fabsf(_motor->FOC.Vdq.q-_motor->m.R*_motor->FOC.Idq_smoothed.q)<HALL_VOLTAGE_THRESHOLD)&&(_motor->FOC.hall_initialised)&&(current_hall_state>0)&&(current_hall_state<7)){
				hall_start_now = 1;
		}else if(fabsf(_motor->FOC.Vdq.q-_motor->m.R*_motor->FOC.Idq_smoothed.q)>HALL_VOLTAGE_THRESHOLD+2.0f){
			hall_start_now = 0;
		}
		if(hall_start_now){
			_motor->FOC.flux_a = 0.01f*_motor->FOC.flux_a + 0.99f*_motor->m.hall_flux[current_hall_state-1][0];
			_motor->FOC.flux_b = 0.01f*_motor->FOC.flux_b + 0.99f*_motor->m.hall_flux[current_hall_state-1][1];
			_motor->FOC.FOCAngle = (uint16_t)(32768.0f + 10430.0f * fast_atan2(_motor->FOC.flux_b, _motor->FOC.flux_a)) - 32768;
		}else{
			flux_observer(_motor);
		}
#else
    	flux_observer(_motor);
#endif
			MESCFOC(_motor);
			writePWM(_motor);
      } else if (_motor->MotorSensorMode == MOTOR_SENSOR_MODE_ENCODER) {
		  _motor->FOC.FOCAngle = _motor->FOC.enc_angle;
		  MESCFOC(_motor);
		  writePWM(_motor);
      } else if(_motor->MotorSensorMode == MOTOR_SENSOR_MODE_OPENLOOP){
		  _motor->FOC.openloop_step = 60;
		  OLGenerateAngle(_motor);
		  MESCFOC(_motor);
		  writePWM(_motor);
      }
      break;

    case MOTOR_STATE_TRACKING:
#ifdef HAS_PHASE_SENSORS
			  // Track using BEMF from phase sensors
			  generateBreak(_motor);
			  getRawADCVph(_motor);
			  ADCPhaseConversion(_motor);
		      MESCTrack(_motor);
		      if (_motor->MotorSensorMode == MOTOR_SENSOR_MODE_HALL) {
		          hallAngleEstimator(_motor);
		          angleObserver(_motor);
		      } else if (_motor->MotorSensorMode == MOTOR_SENSOR_MODE_SENSORLESS) {
		    	  flux_observer(_motor);
#ifdef USE_HALL_START
		    	  HallFluxMonitor(_motor);
#endif
		      } else if (_motor->MotorSensorMode == MOTOR_SENSOR_MODE_ENCODER) {
					_motor->FOC.FOCAngle = _motor->FOC.enc_angle;
		      }
#endif

      break;

    case MOTOR_STATE_OPEN_LOOP_STARTUP:
      // Same as open loop
    	_motor->FOC.openloop_step = 60;
    	OLGenerateAngle(_motor);
    	MESCFOC(_motor);
    	writePWM(_motor);
      // Write the PWM values
      break;

    case MOTOR_STATE_OPEN_LOOP_TRANSITION:
      // Run open loop
      // Run observer
      // RunFOC
      // Weighted average of the outputs N PWM cycles
      // Write the PWM values
      break;

    case MOTOR_STATE_IDLE:
        generateBreak(_motor);
      // Do basically nothing
      break;

    case MOTOR_STATE_DETECTING:

      if ((_motor->hall.current_hall_state == 7)) { // no hall sensors detected, all GPIO pulled high
    	_motor->MotorSensorMode = MOTOR_SENSOR_MODE_SENSORLESS;
        _motor->MotorState = MOTOR_STATE_GET_KV;
      } else if (_motor->hall.current_hall_state == 0) {
        _motor->MotorState = MOTOR_STATE_ERROR;
        MotorError = MOTOR_ERROR_HALL0;
      } else {
        // hall sensors detected
    	  _motor->MotorSensorMode = MOTOR_SENSOR_MODE_HALL;
        getHallTable(_motor);
        MESCFOC(_motor);
        writePWM(_motor);
      }
      break;

    case MOTOR_STATE_MEASURING:
    			// Every PWM cycle we enter this function until
                // the resistance measurement has converged at a
                // good value. Once the measurement is complete,
                // Rphase is set, and this is no longer called
          measureResistance(_motor);
        break;

    case MOTOR_STATE_GET_KV:
      getkV(_motor);

      break;

    case MOTOR_STATE_ERROR:
      generateBreak(_motor);  // Generate a break state (software disabling all PWM)
                        // Now panic and freak out
      break;

    case MOTOR_STATE_ALIGN:
      // Turn on at a given voltage at electricalangle0;
      break;

    case MOTOR_STATE_TEST:
    	if(TestMode == TEST_TYPE_DOUBLE_PULSE){
      // Double pulse test
      doublePulseTest(_motor);
    	}else if(TestMode == TEST_TYPE_DEAD_TIME_IDENT){
    	//Here we are going to pull all phases low, and then increase the duty on one phase until we register a current response.
    	//This duty represents the dead time during which there is no current response
    		getDeadtime(_motor);
    	}else if(TestMode == TEST_TYPE_HARDWARE_VERIFICATION){
    		//Here we want a function that pulls all phases low, then all high and verifies a response
    		//Then we want to show a current response with increasing phase duty
    	}
      break;

    case MOTOR_STATE_RECOVERING:
	      deadshort(_motor); //Function to startup motor from running without phase sensors
      break;

    case MOTOR_STATE_SLAMBRAKE:
      if((fabsf(_motor->Conv.Iu)>input_vars.max_request_Idq.q)||
		  (fabsf(_motor->Conv.Iv)>input_vars.max_request_Idq.q)||
		  (fabsf(_motor->Conv.Iw)>input_vars.max_request_Idq.q)){
    	  generateBreak(_motor);
      }else{
    	  generateEnable(_motor);
    	  htim1.Instance->CCR1 = 0;
    	  htim1.Instance->CCR2 = 0;
    	  htim1.Instance->CCR3 = 0;
    	  //We use "0", since this corresponds to all high side FETs off, always, and all low side ones on, always.
    	  //This means that current measurement can continue on low side and phase shunts, so over current protection remains active.
      }
    break;
    case MOTOR_STATE_RUN_BLDC:
    	getRawADCVph(_motor);
    	ADCPhaseConversion(_motor);
    	BLDCCommute(_motor);
		__NOP();
    	break;

    default:
      _motor->MotorState = MOTOR_STATE_ERROR;
      generateBreak(_motor);
      break;
  }
#ifdef SOFTWARE_ADC_REGULAR
       HAL_ADC_Start(&hadc1); //Try to eliminate the HAL call, slow and inefficient. Leaving this here for now.
        //hadc1.Instance->CR2 |= (uint32_t)ADC_CR2_SWSTART;
#endif
}

// The hyperloop runs at PWM timer bottom, when the PWM is in V7 (all high)
// In this loop, we write the values of the PWM to be updated at the next update
// event (timer top) This is where we want to inject signals for measurement so
// that the next signal level takes affect right after the ADC reading In normal
// run mode, we want to increment the angle and write the next PWM values
static MESCiq_s Idq[2] = {{.d = 0.0f, .q = 0.0f}, {.d = 0.0f, .q = 0.0f}};
static volatile MESCiq_s dIdq = {.d = 0.0f, .q = 0.0f};
//static float IIR[2] = {0.0f, 0.0f};
static MESCiq_s intdidq;
static volatile float nrm, magnitude45, mag45avg;

static volatile float nrm_avg;
static uint16_t last_angle;

void hyperLoop(MESC_motor_typedef *_motor) {
  if ((_motor->FOC.inject)&&(_motor->MotorState != MOTOR_STATE_TRACKING)) {
	  RunHFI(_motor);
  }else {
	  _motor->FOC.Vd_injectionV = 0.0f;
	  _motor->FOC.Vq_injectionV = 0.0f;
//	  _motor->FOC.HFI_int_err = 0.0f;
  }
#ifdef USE_LR_OBSERVER
      LRObserverCollect();
#endif
#ifdef USE_ENCODER
tle5012(_motor);
#endif
//RunPLL for all angle options
_motor->FOC.angle_error = _motor->FOC.angle_error-0.02f*(int16_t)((_motor->FOC.angle_error+(int)(last_angle - _motor->FOC.FOCAngle)));
_motor->FOC.eHz = _motor->FOC.angle_error * _motor->FOC.pwm_frequency/65536.0f;
last_angle = _motor->FOC.FOCAngle;
#ifdef INTERPOLATE_V7_ANGLE
if(fabsf(_motor->FOC.eHz)>0.005f*_motor->FOC.pwm_frequency){
	//Only run it when there is likely to be good speed measurement stability and
	//actual utility in doing it. At low speed, there is minimal benefit, and
	//unstable speed estimation could make it worse.
_motor->FOC.FOCAngle = _motor->FOC.FOCAngle + 0.5f*_motor->FOC.angle_error;
}

#endif
 // _motor->FOC.FOCAngle = _motor->FOC.FOCAngle + _motor->FOC.angle_error;
if(_motor->MotorState==MOTOR_STATE_RUN||_motor->MotorState==MOTOR_STATE_MEASURING){
	writePWM(_motor);
	}
#ifdef LOGGING
if(lognow){
	static int post_error_samples;
	if(_motor->MotorState!=MOTOR_STATE_ERROR){
	logVars(_motor);
	post_error_samples = 50;
	}else{//If we have an error state, we want to keep the data surrounding the error log, including some sampled during and after the fault
		if(post_error_samples>1){
			logVars(_motor);
			post_error_samples--;
		}else if(post_error_samples == 1){
			print_samples_now = 1;
			post_error_samples--;
		}else{
			__NOP();
		}
	}
}
#endif
}

#define MAX_ERROR_COUNT 1

void VICheck(MESC_motor_typedef *_motor) {  // Check currents, voltages are within panic limits


  if (_motor->Raw.Iu > g_hw_setup.RawCurrLim){
	  handleError(_motor, ERROR_OVERCURRENT_PHA);
  }
  if (_motor->Raw.Iv > g_hw_setup.RawCurrLim){
	  handleError(_motor, ERROR_OVERCURRENT_PHB);
  }
  if (_motor->Raw.Iw > g_hw_setup.RawCurrLim){
	  handleError(_motor,ERROR_OVERCURRENT_PHC);
  }
  if (_motor->Raw.Vbus > g_hw_setup.RawVoltLim){
	  handleError(_motor, ERROR_OVERVOLTAGE);
  }
}
float maxIgamma;
uint16_t phasebalance;
  void ADCConversion(MESC_motor_typedef *_motor) {
	  _motor->FOC.Idq_smoothed.d = (_motor->FOC.Idq_smoothed.d*99.0f + _motor->FOC.Idq.d)*0.01f;
	  _motor->FOC.Idq_smoothed.q = (_motor->FOC.Idq_smoothed.q*99.0f + _motor->FOC.Idq.q)*0.01f;

    getRawADC(_motor);

    // Here we take the raw ADC values, offset, cast to (float) and use the
    // hardware gain values to create volt and amp variables
    //Convert the currents to real amps in SI units
	_motor->Conv.Iu =
		(float)(_motor->Raw.Iu - _motor->offset.Iu) * g_hw_setup.Igain;
	_motor->Conv.Iv =
		(float)(_motor->Raw.Iv - _motor->offset.Iv) * g_hw_setup.Igain;
	_motor->Conv.Iw =
		(float)(_motor->Raw.Iw - _motor->offset.Iw) * g_hw_setup.Igain;
	_motor->Conv.Vbus =
		(float)_motor->Raw.Vbus * g_hw_setup.VBGain;  // Vbus

    //Check for over limit conditions. We want this after the conversion so that the
    //correct overcurrent values are logged
    //VICheck(_motor); //This uses the "raw" values, and requires an extra function call
    if (_motor->Conv.Iu > g_hw_setup.Imax){
  	  handleError(_motor, ERROR_OVERCURRENT_PHA);}
    if (_motor->Conv.Iv > g_hw_setup.Imax){
  	  handleError(_motor, ERROR_OVERCURRENT_PHB);}
    if (_motor->Conv.Iw > g_hw_setup.Imax){
  	  handleError(_motor,ERROR_OVERCURRENT_PHC);}
    if (_motor->Conv.Vbus > g_hw_setup.Vmax){
  	  handleError(_motor, ERROR_OVERVOLTAGE);}

//Deal with terrible hardware choice of only having two current sensors
//Based on Iu+Iv+Iw = 0
#ifdef MISSING_UCURRSENSOR
    _motor->Conv.Iu =
    		-_motor->Conv.Iv -_motor->Conv.Iw;
#endif
#ifdef MISSING_VCURRSENSOR
    _motor->Conv.Iv =
    		-_motor->Conv.Iu -_motor->Conv.Iw;
#endif
#ifdef MISSING_WCURRSENSOR
    _motor->Conv.Iw =
    		-_motor->Conv.Iu -_motor->Conv.Iv;
#endif


    // Power Variant Clark transform
    // Here we select the phases that have the lowest duty cycle to us, since
    // they should have the best current measurements
    if (htim1.Instance->CCR1 > _motor->FOC.ADC_duty_threshold) {
      // Clark using phase V and W
      _motor->FOC.Iab.a = -_motor->Conv.Iv -
    		  _motor->Conv.Iw;
      _motor->FOC.Iab.b =
          one_on_sqrt3 * _motor->Conv.Iv -
          one_on_sqrt3 * _motor->Conv.Iw;
    } else if (htim1.Instance->CCR2 > _motor->FOC.ADC_duty_threshold) {
      // Clark using phase U and W
      _motor->FOC.Iab.a = _motor->Conv.Iu;
      _motor->FOC.Iab.b =
          -one_on_sqrt3 * _motor->Conv.Iu -
          two_on_sqrt3 * _motor->Conv.Iw;
    } else if (htim1.Instance->CCR3 > _motor->FOC.ADC_duty_threshold) {
      // Clark using phase U and V
      _motor->FOC.Iab.a = _motor->Conv.Iu;
      _motor->FOC.Iab.b =
          two_on_sqrt3 * _motor->Conv.Iv +
          one_on_sqrt3 * two_on_sqrt3 *
		  _motor->Conv.Iu;
    } else {
#ifdef USE_HIGHHOPES_PHASE_BALANCING
		_motor->FOC.Iab.b = _motor->Conv.Iu + _motor->Conv.Iv + _motor->Conv.Iw;
if(phasebalance){
	_motor->Conv.Iu = _motor->Conv.Iu + _motor->FOC.Iab.b;
	_motor->Conv.Iv = _motor->Conv.Iu + _motor->FOC.Iab.b;
		m_motor->Conv.Iw = _motor->Conv.Iu + _motor->FOC.Iab.b;
		}
		if(fabs(_motor->FOC.Iab.b)>fabs(maxIgamma)){
			maxIgamma = _motor->FOC.Iab.b;
		}
		if(_motor->FOC.Vdq.q<2.0f){ //Reset it to reject accumulated random noise and enable multiple goes
			maxIgamma = 0.0f;
		}
#endif
      // Do the full transform
	      _motor->FOC.Iab.a =
	          0.66666f * _motor->Conv.Iu -
	          0.33333f * _motor->Conv.Iv -
	          0.33333f * _motor->Conv.Iw;
	      _motor->FOC.Iab.b =
	          one_on_sqrt3 * _motor->Conv.Iv -
	          one_on_sqrt3 * _motor->Conv.Iw;
    }

    // Park
    _motor->FOC.Idq.d = _motor->FOC.sincosangle.cos * _motor->FOC.Iab.a +
                     _motor->FOC.sincosangle.sin * _motor->FOC.Iab.b;
    _motor->FOC.Idq.q = _motor->FOC.sincosangle.cos * _motor->FOC.Iab.b -
                     _motor->FOC.sincosangle.sin * _motor->FOC.Iab.a;
  }

  void ADCPhaseConversion(MESC_motor_typedef *_motor) {
	  //To save clock cycles in the main run loop we only want to convert the phase voltages while tracking.
  //Convert the voltages to volts in real SI units
	  _motor->Conv.Vu = (float)_motor->Raw.Vu * g_hw_setup.VBGain;
	  _motor->Conv.Vv = (float)_motor->Raw.Vv * g_hw_setup.VBGain;
	  _motor->Conv.Vw = (float)_motor->Raw.Vw * g_hw_setup.VBGain;
  }

  /////////////////////////////////////////////////////////////////////////////
  // SENSORLESS IMPLEMENTATION//////////////////////////////////////////////////


  void flux_observer(MESC_motor_typedef *_motor) {
    // LICENCE NOTE REMINDER:
    // This work deviates slightly from the BSD 3 clause licence.
    // The work here is entirely original to the MESC FOC project, and not based
    // on any appnotes, or borrowed from another project. This work is free to
    // use, as granted in BSD 3 clause, with the exception that this note must
    // be included in where this code is implemented/modified to use your
    // variable names, structures containing variables or other minor
    // rearrangements in place of the original names I have chosen, and credit
    // to David Molony as the original author must be noted.

    // With thanks to C0d3b453 for generally keeping this compiling and Elwin
    // for producing data comparing the output to a 16bit encoder.

#ifdef USE_FLUX_LINKAGE_OBSERVER
	  //Variant of the flux linkage observer created by/with Benjamin Vedder to
	  //eliminate the need to accurately know the flux linked motor parameter.
	  //This may be useful when approaching saturation; currently unclear but
	  //definitely makes setup less critical.
	  //It basically takes the normal of the flux linkage at any time and
	  //changes the flux limits accordingly, ignoring using a sqrt for computational efficiency
	  float flux_linked_norm = _motor->FOC.flux_a*_motor->FOC.flux_a+_motor->FOC.flux_b*_motor->FOC.flux_b;
	  float flux_err = flux_linked_norm-_motor->FOC.flux_observed*_motor->FOC.flux_observed;
	  _motor->FOC.flux_observed = _motor->FOC.flux_observed+ _motor->m.flux_linkage_gain*flux_err;
	  if(_motor->FOC.flux_observed>_motor->m.flux_linkage_max){_motor->FOC.flux_observed = _motor->m.flux_linkage_max;}
	  if(_motor->FOC.flux_observed<_motor->m.flux_linkage_min){_motor->FOC.flux_observed = _motor->m.flux_linkage_min;}
#endif
	// This is the actual observer function.
	// We are going to integrate Va-Ri and clamp it positively and negatively
	// the angle is then the arctangent of the integrals shifted 180 degrees
#ifdef USE_SALIENT_OBSERVER
	  float La, Lb;
	  getLabFast(_motor->FOC.FOCAngle, _motor->m.L_D, _motor->m.L_QD, &La, &Lb);

	  _motor->FOC.flux_a = _motor->FOC.flux_a +
			  (_motor->FOC.Vab.a - _motor->m.R * _motor->FOC.Iab.a)*_motor->FOC.pwm_period -
        La * (_motor->FOC.Iab.a - _motor->FOC.Ia_last) - //Salient inductance NOW
		_motor->FOC.Iab.a * (La - La_last); //Differential of phi = Li -> Ldi/dt+idL/dt
	  _motor->FOC.flux_b = _motor->FOC.flux_b +
			  (_motor->FOC.Vab.b - _motor->m.R * _motor->FOC.Iab.b)*_motor->FOC.pwm_period -
        Lb * (_motor->FOC.Iab.b - _motor->FOC.Ib_last) -
		_motor->FOC.Iab.b * (Lb-Lb_last);
//Store the inductances
    La_last = La;
    Lb_last = Lb;
#else
	  _motor->FOC.flux_a =
			  _motor->FOC.flux_a + (_motor->FOC.Vab.a - _motor->m.R * _motor->FOC.Iab.a)*_motor->FOC.pwm_period-
        _motor->m.L_D * (_motor->FOC.Iab.a - _motor->FOC.Ia_last);
	  _motor->FOC.flux_b =
			  _motor->FOC.flux_b + (_motor->FOC.Vab.b - _motor->m.R * _motor->FOC.Iab.b)*_motor->FOC.pwm_period -
        _motor->m.L_D * (_motor->FOC.Iab.b - _motor->FOC.Ib_last);
#endif
//Store the currents
    _motor->FOC.Ia_last = _motor->FOC.Iab.a;
    _motor->FOC.Ib_last = _motor->FOC.Iab.b;

#ifdef USE_NONLINEAR_OBSERVER_CENTERING
///Try directly applying the centering using the same method as the flux linkage observer
    float err = _motor->FOC.flux_observed*_motor->FOC.flux_observed-_motor->FOC.flux_a*_motor->FOC.flux_a-_motor->FOC.flux_b*_motor->FOC.flux_b;
    _motor->FOC.flux_b = _motor->FOC.flux_b+err*_motor->FOC.flux_b*_motor->m.non_linear_centering_gain;
    _motor->FOC.flux_a = _motor->FOC.flux_a+err*_motor->FOC.flux_a*_motor->m.non_linear_centering_gain;
#endif
#ifdef USE_CLAMPED_OBSERVER_CENTERING
    if (_motor->FOC.flux_a > _motor->FOC.flux_observed) {
    	_motor->FOC.flux_a = _motor->FOC.flux_observed;}
    if (_motor->FOC.flux_a < -_motor->FOC.flux_observed) {
    	_motor->FOC.flux_a = -_motor->FOC.flux_observed;}
    if (_motor->FOC.flux_b > _motor->FOC.flux_observed) {
    	_motor->FOC.flux_b = _motor->FOC.flux_observed;}
    if (_motor->FOC.flux_b < -_motor->FOC.flux_observed) {
    	_motor->FOC.flux_b = -_motor->FOC.flux_observed;}
#endif

    if(_motor->FOC.inject==0){
    	_motor->FOC.FOCAngle = (uint16_t)(32768.0f + 10430.0f * fast_atan2(_motor->FOC.flux_b, _motor->FOC.flux_a)) - 32768;
    }


#ifdef USE_ENCODER
    //This does not apply the encoder angle,
    //It tracks the difference between the encoder and the observer.
    _motor->FOC.enc_obs_angle = _motor->FOC.FOCAngle - _motor->FOC.enc_angle;
#endif
  }

  // fast_atan2 based on https://math.stackexchange.com/a/1105038/81278
  // Via Odrive project
  // https://github.com/odriverobotics/ODrive/blob/master/Firmware/MotorControl/utils.cpp
  // This function is MIT licenced, copyright Oskar Weigl/Odrive Robotics
  // The origin for Odrive atan2 is public domain. Thanks to Odrive for making
  // it easy to borrow.
  float min(float lhs, float rhs) { return (lhs < rhs) ? lhs : rhs; }
  float max(float lhs, float rhs) { return (lhs > rhs) ? lhs : rhs; }

  float fast_atan2(float y, float x) {
    // a := min (|x|, |y|) / max (|x|, |y|)
    float abs_y = fabsf(y);
    float abs_x = fabsf(x);
    // inject FLT_MIN in denominator to avoid division by zero
    float a = min(abs_x, abs_y) / (max(abs_x, abs_y));
    // s := a * a
    float s = a * a;
    // r := ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a
    float r =
        ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    // if |y| > |x| then r := 1.57079637 - r
    if (abs_y > abs_x) r = 1.57079637f - r;
    // if x < 0 then r := 3.14159274 - r
    if (x < 0.0f) r = 3.14159274f - r;
    // if y < 0 then r := -r
    if (y < 0.0f) r = -r;

    return r;
  }

  /////////////////////////////////////////////////////////////////////////////
  ////////Hall Sensor Implementation///////////////////////////////////////////

  void hallAngleEstimator(MESC_motor_typedef *_motor) {  // Implementation using the mid point of the hall
                               // sensor angles, which should be much more
                               // reliable to generate that the edges

    if (_motor->hall.current_hall_state != _motor->hall.last_hall_state) {
      _motor->FOC.hall_update = 1;
      if (_motor->hall.current_hall_state == 0) {
        _motor->MotorState = MOTOR_STATE_ERROR;
        MotorError = MOTOR_ERROR_HALL0;
      } else if (_motor->hall.current_hall_state == 7) {
        _motor->MotorState = MOTOR_STATE_ERROR;
        MotorError = MOTOR_ERROR_HALL7;
      }
      //////////Implement the Hall table here, but the vector can be dynamically
      /// created/filled by another function/////////////
      _motor->hall.current_hall_angle = _motor->m.hall_table[_motor->hall.current_hall_state - 1][2];

      // Calculate Hall error

      uint16_t a;
      if ((a = _motor->hall.current_hall_angle - _motor->hall.last_hall_angle) < 32000)  // Forwards
      {
    	  _motor->hall.hall_error =
    			  _motor->FOC.FOCAngle - _motor->m.hall_table[_motor->hall.current_hall_state - 1][0];
    	  _motor->hall.dir = 1.0f;
        // _motor->FOC.HallAngle = _motor->FOC.HallAngle - 5460;
      } else  // Backwards
      {
    	_motor->hall.hall_error =
    			_motor->FOC.FOCAngle - _motor->m.hall_table[_motor->hall.current_hall_state - 1][1];
    	_motor->hall.dir = -1.0f;
        // _motor->FOC.HallAngle = _motor->FOC.HallAngle + 5460;
      }
      if (_motor->hall.hall_error > 32000) {
    	  _motor->hall.hall_error = _motor->hall.hall_error - 65536;
      }
      if (_motor->hall.hall_error < -32000) {
    	  _motor->hall.hall_error = _motor->hall.hall_error + 65536;
      }
    }
  }

  void angleObserver(MESC_motor_typedef *_motor) {
    // This function should take the available data (hall change, BEMF crossing
    // etc...) and process it with a PLL type mechanism
    if (_motor->FOC.hall_update == 1) {
      _motor->FOC.hall_update = 0;
      _motor->hall.last_observer_period = _motor->hall.ticks_since_last_observer_change;
      float one_on_ticks = (1.0f / _motor->hall.ticks_since_last_observer_change);
      _motor->hall.one_on_last_observer_period =
          (4.0f * _motor->hall.one_on_last_observer_period + (one_on_ticks)) * 0.2f;  // ;
      _motor->hall.angle_step =
          (4.0f * _motor->hall.angle_step +
           (one_on_ticks)*_motor->m.hall_table[_motor->hall.last_hall_state - 1][3]) *
          0.2f;

      // Reset the counters, track the previous state
      _motor->hall.last_hall_state = _motor->hall.current_hall_state;
      _motor->hall.last_hall_angle = _motor->hall.current_hall_angle;
      _motor->hall.ticks_since_last_observer_change = 0;
    }

    // Run the counter
    _motor->hall.ticks_since_last_observer_change = _motor->hall.ticks_since_last_observer_change + 1;

    if (_motor->hall.ticks_since_last_observer_change <= 2.0f * _motor->hall.last_observer_period) {
      /*      _motor->FOC.FOCAngle = _motor->FOC.FOCAngle + (uint16_t)(dir*angle_step
         + one_on_last_observer_period * (-0.9f * hall_error)); //Does not
         work...
           //Why?
 */
      if (_motor->hall.dir > 0) {  // Apply a gain to the error as well as the feed forward
        // from the last hall period. Gain of 0.9-1.1 seems to work
        // well when using corrected hall positions and spacings
        _motor->FOC.FOCAngle =
            _motor->FOC.FOCAngle +
            (uint16_t)(_motor->hall.angle_step - _motor->hall.one_on_last_observer_period * _motor->hall.hall_error);
        // one_on_last_observer_period * (-0.2f * hall_error));
      } else if (_motor->hall.dir < 0.0f) {
        _motor->FOC.FOCAngle =
            _motor->FOC.FOCAngle +
            (uint16_t)(-_motor->hall.angle_step +
            		_motor->hall.one_on_last_observer_period * (-0.9f * _motor->hall.hall_error));
        // Also does not work,
        // Why??
        _motor->FOC.FOCAngle =
            _motor->FOC.FOCAngle -
            (uint16_t)(_motor->hall.angle_step +
            		_motor->hall.one_on_last_observer_period * (0.2f * _motor->hall.hall_error));
      }
    }
    if (_motor->hall.ticks_since_last_observer_change > 1500.0f) {
    	_motor->hall.ticks_since_last_observer_change = 1500.0f;
    	_motor->hall.last_observer_period = 1500.0f;  //(ticks_since_last_hall_change);
    	_motor->hall.one_on_last_observer_period =
          1.0f / _motor->hall.last_observer_period;  // / ticks_since_last_hall_change;
      _motor->FOC.FOCAngle = _motor->hall.current_hall_angle;
    }
  }

  void OLGenerateAngle(MESC_motor_typedef *_motor) {

    _motor->FOC.FOCAngle = _motor->FOC.FOCAngle + _motor->FOC.openloop_step;
    // ToDo
  }
  /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // FOC PID algorithms
  //////////////////////////////////////////////////////////////////////////////////////////


  void MESCFOC(MESC_motor_typedef *_motor) {

    // Here we are going to do a PID loop to control the dq currents, converting
    // Idq into Vdq Calculate the errors
    static MESCiq_s Idq_err;
#if defined(USE_FIELD_WEAKENING) || defined(USE_FIELD_WEAKENINGv2)
    if((_motor->FOC.FW_current<_motor->FOC.Idq_req.d)&&(_motor->MotorState==MOTOR_STATE_RUN)){//Field weakenning is -ve, but there may already be d-axis from the MTPA
    	Idq_err.d = (_motor->FOC.FW_current - _motor->FOC.Idq.d) * _motor->FOC.Id_pgain;
    }else{
    	Idq_err.d = (_motor->FOC.Idq_req.d - _motor->FOC.Idq.d) * _motor->FOC.Id_pgain;
    }
#else
	Idq_err.d = (_motor->FOC.Idq_req.d - _motor->FOC.Idq.d) * _motor->FOC.Id_pgain;
#endif
    Idq_err.q = (_motor->FOC.Idq_req.q - _motor->FOC.Idq.q) * _motor->FOC.Iq_pgain;

    // Integral error
    _motor->FOC.Idq_int_err.d =
    		_motor->FOC.Idq_int_err.d + _motor->FOC.Id_igain * Idq_err.d * _motor->FOC.pwm_period;
    _motor->FOC.Idq_int_err.q =
    		_motor->FOC.Idq_int_err.q + _motor->FOC.Iq_igain * Idq_err.q * _motor->FOC.pwm_period;
    // Apply the integral gain at this stage to enable bounding it


        // Apply the PID, and potentially smooth the output for noise - sudden
      // changes in VDVQ may be undesirable for some motors. Integral error is
      // pre-bounded to avoid integral windup, proportional gain needs to have
      // effect even at max integral to stabilise and avoid trips
      _motor->FOC.Vdq.d = Idq_err.d + _motor->FOC.Idq_int_err.d;
      _motor->FOC.Vdq.q = Idq_err.q + _motor->FOC.Idq_int_err.q;

      // Bounding final output

#if defined(USE_SQRT_CIRCLE_LIM)
      float Vmagnow2 = _motor->FOC.Vdq.d*_motor->FOC.Vdq.d+_motor->FOC.Vdq.q*_motor->FOC.Vdq.q;
      //Check if the vector length is greater than the available voltage
      if(Vmagnow2>_motor->FOC.Vmag_max2){
		  float Vmagnow = sqrtf(Vmagnow2);
		  float one_on_Vmagnow = 1.0f/Vmagnow;
		  float one_on_VmagnowxVmagmax = _motor->FOC.Vmag_max*one_on_Vmagnow;
		  _motor->FOC.Vdq.d = _motor->FOC.Vdq.d*one_on_VmagnowxVmagmax;
		  _motor->FOC.Vdq.q = _motor->FOC.Vdq.q*one_on_VmagnowxVmagmax;
		  _motor->FOC.Idq_int_err.d = _motor->FOC.Idq_int_err.d*one_on_VmagnowxVmagmax;
		  _motor->FOC.Idq_int_err.q = _motor->FOC.Idq_int_err.q*one_on_VmagnowxVmagmax;
#ifdef USE_FIELD_WEAKENINGv2
		  _motor->FOC.FW_current = _motor->FOC.FW_current -0.01f*_motor->FOC.field_weakening_curr_max;
      }else{
		  _motor->FOC.FW_current = _motor->FOC.FW_current +0.01f*_motor->FOC.field_weakening_curr_max;
      }
      if(_motor->FOC.FW_current>0.0f){_motor->FOC.FW_current=0.0f;}
      if(_motor->FOC.FW_current<-_motor->FOC.field_weakening_curr_max){_motor->FOC.FW_current= -_motor->FOC.field_weakening_curr_max;}
#else
  } //Just close the bracket
#endif
#elif defined(USE_SQRT_CIRCLE_LIM_VD)
     //Circle limiter that favours Vd, similar to used in VESC, and as an option in ST firmware.for torque
      //This method was primarily designed for induction motors, where the d axis is required to
      //make the magnetic field for torque. Nevertheless, this finds application at extreme currents and
      //during field weakening.
      //Latent concerns about the usual implementation that allows ALL the voltage to be
      //assigned to Vd becoming unstable as the angle relative to the rotor exceeds 45 degrees
      //due to rapidly collapsing q-axis voltage. Therefore, this option will be allowed, but
      // with a limit of voltage angle 60degrees (sin60 = 0.866) from the rotor.

      if(_motor->FOC.Vdq.d<-0.866f*_motor->FOC.Vmag_max){ //Negative values of Vd - Normally Vd is -ve since it is driving field advance
		  _motor->FOC.Vdq.d = -0.866f*_motor->FOC.Vmag_max; //Hard clamp the Vd
		  if(_motor->FOC.Idq_int_err.d<_motor->FOC.Vdq.d){
			  _motor->FOC.Idq_int_err.d = _motor->FOC.Vdq.d; //Also clamp the integral to stop windup
		  }
      } else if(_motor->FOC.Vdq.d>0.866f*_motor->FOC.Vmag_max){ //Positive values of Vd
    	  _motor->FOC.Vdq.d = 0.866f*_motor->FOC.Vmag_max; //Hard clamp the Vd
		  if(_motor->FOC.Idq_int_err.d>_motor->FOC.Vdq.d){
			  _motor->FOC.Idq_int_err.d = _motor->FOC.Vdq.d; //Also clamp the integral to stop windup
		  }
      }

      //Now we take care of the overall length of the voltage vector
      float Vmagnow2 = _motor->FOC.Vdq.d*_motor->FOC.Vdq.d+_motor->FOC.Vdq.q*_motor->FOC.Vdq.q;
      if(Vmagnow2>_motor->FOC.Vmag_max2){
    	  if(_motor->FOC.Vdq.q>0.0f){ //Positive Vq
    		  _motor->FOC.Vdq.q = sqrtf(_motor->FOC.Vmag_max2-_motor->FOC.Vdq.d*_motor->FOC.Vdq.d);
    		  if(_motor->FOC.Idq_int_err.q>_motor->FOC.Vdq.q){
    			  _motor->FOC.Idq_int_err.q = _motor->FOC.Vdq.q;
    		  }
    	  }
    	  else{ //Negative Vq
    		  _motor->FOC.Vdq.q = -sqrtf(_motor->FOC.Vmag_max2-_motor->FOC.Vdq.d*_motor->FOC.Vdq.d);
			  if(_motor->FOC.Idq_int_err.q<_motor->FOC.Vdq.q){
				  _motor->FOC.Idq_int_err.q = _motor->FOC.Vdq.q;
			  }
    	  }
#ifdef USE_FIELD_WEAKENINGv2
//I have no idea if this is a good idea or not, but it makes the motor a lot faster.
    	  _motor->FOC.FW_current = _motor->FOC.FW_current -0.01f*_motor->FOC.field_weakening_curr_max;
      }else{
		  _motor->FOC.FW_current = _motor->FOC.FW_current +0.01f*_motor->FOC.field_weakening_curr_max;
      }
      if(_motor->FOC.FW_current>0.0f){_motor->FOC.FW_current=0.0f;}
      if(_motor->FOC.FW_current<-_motor->FOC.field_weakening_curr_max){_motor->FOC.FW_current= -_motor->FOC.field_weakening_curr_max;}
#else
  } //Just close the circle limiter bracket
#endif
#else
      // These limits are experimental, but result in close to 100% modulation.
      // Since Vd and Vq are orthogonal, limiting Vd is not especially helpful
      // in reducing overall voltage magnitude, since the relation
      // Vout=(Vd^2+Vq^2)^0.5 results in Vd having a small effect. Vd is
      // primarily used to drive the resistive part of the field; there is no
      // BEMF pushing against Vd and so it does not scale with RPM (except for
      // cross coupling).

      // Bounding integral
        if (_motor->FOC.Idq_int_err.d > _motor->FOC.Vdint_max){_motor->FOC.Idq_int_err.d = _motor->FOC.Vdint_max;}
        if (_motor->FOC.Idq_int_err.d < -_motor->FOC.Vdint_max){_motor->FOC.Idq_int_err.d = -_motor->FOC.Vdint_max;}
        if (_motor->FOC.Idq_int_err.q > _motor->FOC.Vqint_max){_motor->FOC.Idq_int_err.q = _motor->FOC.Vqint_max;}
        if (_motor->FOC.Idq_int_err.q < -_motor->FOC.Vqint_max){_motor->FOC.Idq_int_err.q = -_motor->FOC.Vqint_max;}
      //Bounding output
      if (_motor->FOC.Vdq.d > _motor->FOC.Vd_max)
        (_motor->FOC.Vdq.d = _motor->FOC.Vd_max);
      if (_motor->FOC.Vdq.d < -_motor->FOC.Vd_max)
        (_motor->FOC.Vdq.d = -_motor->FOC.Vd_max);
      if (_motor->FOC.Vdq.q > _motor->FOC.Vq_max)
        (_motor->FOC.Vdq.q = _motor->FOC.Vq_max);
      if (_motor->FOC.Vdq.q < -_motor->FOC.Vq_max)
        (_motor->FOC.Vdq.q = -_motor->FOC.Vq_max);
#endif
#ifdef USE_FIELD_WEAKENING
      //Calculate the module of voltage applied,
      Vmagnow2 = _motor->FOC.Vdq.d*_motor->FOC.Vdq.d+_motor->FOC.Vdq.q*_motor->FOC.Vdq.q; //Need to recalculate this since limitation has maybe been applied
      //Apply a linear slope from the threshold to the max module
      //Step towards with exponential smoother
      if(Vmagnow2>(_motor->FOC.field_weakening_threshold*_motor->FOC.field_weakening_threshold)){
    	  _motor->FOC.FW_current = 0.95f*_motor->FOC.FW_current +
    			  	  	0.05f*_motor->FOC.field_weakening_curr_max *_motor->FOC.field_weakening_multiplier*
						(_motor->FOC.field_weakening_threshold - sqrtf(Vmagnow2));
      }else{
    	  _motor->FOC.FW_current*=0.95f;//Ramp down a bit slowly
		  if(_motor->FOC.FW_current>0.1f){//We do not allow positive field weakening current, and we want it to actually go to zero eventually
			  _motor->FOC.FW_current = 0.0f;
		  }
      }
      //Apply the field weakening only if the additional d current is greater than the requested d current

#endif
    }


  static float mid_value = 0;
  float top_value;
  float bottom_value;
 uint16_t deadtime_comp = DEADTIME_COMP_V;

 void writePWM(MESC_motor_typedef *_motor) {
float Vd, Vq;

		 Vd = _motor->FOC.Vdq.d + _motor->FOC.Vd_injectionV;
		 Vq = _motor->FOC.Vdq.q + _motor->FOC.Vq_injectionV;

    // Now we update the sin and cos values, since when we do the inverse
    // transforms, we would like to use the most up to date versions(or even the
    // next predicted version...)
	sin_cos_fast(_motor->FOC.FOCAngle, &_motor->FOC.sincosangle.sin, &_motor->FOC.sincosangle.cos);

    // Inverse Park transform
    _motor->FOC.Vab.a = _motor->FOC.sincosangle.cos * Vd -
                      _motor->FOC.sincosangle.sin * Vq;
    _motor->FOC.Vab.b = _motor->FOC.sincosangle.sin * Vd +
                      _motor->FOC.sincosangle.cos * Vq;

	// Inverse Clark transform - power variant
	_motor->FOC.inverterVoltage[0] = _motor->FOC.Vab.a;
	_motor->FOC.inverterVoltage[1] = -0.5f*_motor->FOC.inverterVoltage[0];
	_motor->FOC.inverterVoltage[2] = _motor->FOC.inverterVoltage[1] - sqrt3_on_2 * _motor->FOC.Vab.b;
	_motor->FOC.inverterVoltage[1] = _motor->FOC.inverterVoltage[1] + sqrt3_on_2 * _motor->FOC.Vab.b;

    ////////////////////////////////////////////////////////
    // SVPM implementation
    // Try to do this as a "midpoint clamp" where rather than finding the
    // lowest, we find the highest and lowest and subtract the middle
    top_value = _motor->FOC.inverterVoltage[0];
    bottom_value = top_value;

    if (_motor->FOC.inverterVoltage[1] > top_value) {
      top_value = _motor->FOC.inverterVoltage[1];
    }
    if (_motor->FOC.inverterVoltage[2] > top_value) {
      top_value = _motor->FOC.inverterVoltage[2];
    }
    if (_motor->FOC.inverterVoltage[1] < bottom_value) {
      bottom_value = _motor->FOC.inverterVoltage[1];
    }
    if (_motor->FOC.inverterVoltage[2] < bottom_value) {
      bottom_value = _motor->FOC.inverterVoltage[2];
    }
#ifdef SEVEN_SECTOR
    mid_value = _motor->FOC.PWMmid -
                0.5f * _motor->FOC.Vab_to_PWM * (top_value + bottom_value);

    ////////////////////////////////////////////////////////
    // Actually write the value to the timer registers
    _motor->mtimer->Instance->CCR1 =
    		(uint16_t)(_motor->FOC.Vab_to_PWM * _motor->FOC.inverterVoltage[0] + mid_value);
    _motor->mtimer->Instance->CCR2 =
    		(uint16_t)(_motor->FOC.Vab_to_PWM * _motor->FOC.inverterVoltage[1] + mid_value);
    _motor->mtimer->Instance->CCR3 =
    		(uint16_t)(_motor->FOC.Vab_to_PWM * _motor->FOC.inverterVoltage[2] + mid_value);

    //Dead time compensation
#ifdef DEADTIME_COMP
    // LICENCE NOTE:
    	  // This function deviates slightly from the BSD 3 clause licence.
    	  // The work here is entirely original to the MESC FOC project, and not based
    	  // on any appnotes, or borrowed from another project. This work is free to
    	  // use, as granted in BSD 3 clause, with the exception that this note must
    	  // be included in where this code is implemented/modified to use your
    	  // variable names, structures containing variables or other minor
    	  // rearrangements in place of the original names I have chosen, and credit
    	  // to David Molony as the original author must be noted.
    //The problem with dead time, is that it is essentially a voltage tie through the body diodes to VBus or ground, depending on the current direction.
    //If we know the direction of current, and the effective dead time length we can remove this error, by writing the corrected voltage.
    //This is observed to improve sinusoidalness of currents, but has a slight audible buzz
    //When the current is approximately zero, it is hard to resolve the direction, and therefore the compensation is ineffective.
    //However, no torque is generated when the current and voltage are close to zero, so no adverse performance except the buzz.
    if(_motor->Conv.Iu < -0.030f){_motor->mtimer->Instance->CCR1 = _motor->mtimer->Instance->CCR1-deadtime_comp;}
    if(_motor->Conv.Iv < -0.030f){_motor->mtimer->Instance->CCR2 = _motor->mtimer->Instance->CCR2-deadtime_comp;}
    if(_motor->Conv.Iw < -0.030f){_motor->mtimer->Instance->CCR3 = _motor->mtimer->Instance->CCR3-deadtime_comp;}
    if(_motor->Conv.Iu > -0.030f){_motor->mtimer->Instance->CCR1 = _motor->mtimer->Instance->CCR1+deadtime_comp;}
    if(_motor->Conv.Iv > -0.030f){_motor->mtimer->Instance->CCR2 = _motor->mtimer->Instance->CCR2+deadtime_comp;}
    if(_motor->Conv.Iw > -0.030f){_motor->mtimer->Instance->CCR3 = _motor->mtimer->Instance->CCR3+deadtime_comp;}

#endif
#else //Use 5 sector, bottom clamp implementation
//ToDo, threshold for turning on sinusoidal modulation
    _motor->FOC.inverterVoltage[0] = _motor->FOC.inverterVoltage[0]-bottom_value;
    _motor->FOC.inverterVoltage[1] = _motor->FOC.inverterVoltage[1]-bottom_value;
    _motor->FOC.inverterVoltage[2] = _motor->FOC.inverterVoltage[2]-bottom_value;

    _motor->mtimer->Instance->CCR1 = (uint16_t)(_motor->FOC.Vab_to_PWM * _motor->FOC.inverterVoltage[0]);
    _motor->mtimer->Instance->CCR2 = (uint16_t)(_motor->FOC.Vab_to_PWM * _motor->FOC.inverterVoltage[1]);
    _motor->mtimer->Instance->CCR3 = (uint16_t)(_motor->FOC.Vab_to_PWM * _motor->FOC.inverterVoltage[2]);
#ifdef OVERMOD_DT_COMP_THRESHOLD
    //Concept here is that if we are close to the VBus max, we just do not turn the FET off.
    //Set CCRx to ARR, record how much was added, then next cycle, remove it from the count.
    //If the duty is still above the threshold, the CCR will still be set to ARR, until the duty request is sufficiently low...
static int carryU, carryV, carryW;

	_motor->mtimer->Instance->CCR1 = 	_motor->mtimer->Instance->CCR1 - carryU;
	_motor->mtimer->Instance->CCR2 = 	_motor->mtimer->Instance->CCR2 - carryV;
	_motor->mtimer->Instance->CCR3 = 	_motor->mtimer->Instance->CCR3 - carryW;
	carryU = 0;
	carryV = 0;
	carryW = 0;

	if(_motor->mtimer->Instance->CCR1>(_motor->mtimer->Instance->ARR-OVERMOD_DT_COMP_THRESHOLD)){
		carryU = _motor->mtimer->Instance->ARR-_motor->mtimer->Instance->CCR1; //Save the amount we have overmodulated by
		_motor->mtimer->Instance->CCR1 = _motor->mtimer->Instance->ARR;
	}
	if(_motor->mtimer->Instance->CCR2>(_motor->mtimer->Instance->ARR-OVERMOD_DT_COMP_THRESHOLD)){
		carryV = _motor->mtimer->Instance->ARR-_motor->mtimer->Instance->CCR2; //Save the amount we have overmodulated by
		_motor->mtimer->Instance->CCR2 = _motor->mtimer->Instance->ARR;
	}
	if(_motor->mtimer->Instance->CCR3>(_motor->mtimer->Instance->ARR-OVERMOD_DT_COMP_THRESHOLD)){
		carryW = _motor->mtimer->Instance->ARR-_motor->mtimer->Instance->CCR3; //Save the amount we have overmodulated by
		_motor->mtimer->Instance->CCR3 = _motor->mtimer->Instance->ARR;
	}
#endif
#endif


  }

  // Here we set all the PWMoutputs to LOW, without triggering the timerBRK,
  // which should only be set by the hardware comparators, in the case of a
  // shoot-through or other catastrophic event This function means that the
  // timer can be left running, ADCs sampling etc which enables a recovery, or
  // single PWM period break in which the backEMF can be measured directly
  // This function needs implementing and testing before any high current or
  // voltage is applied, otherwise... DeadFETs
  void generateBreak(MESC_motor_typedef *_motor) {
    phU_Break(_motor);
    phV_Break(_motor );
    phW_Break(_motor );
  }
  void generateEnable(MESC_motor_typedef *_motor) {
    phU_Enable(_motor);
    phV_Enable(_motor);
    phW_Enable(_motor);
  }

  void generateBreakAll() {
    for(int i=0;i<NUM_MOTORS;i++){
    	generateBreak(&mtr[i]);
    }
  }


 void measureResistance(MESC_motor_typedef *_motor) {
    if (_motor->meas.PWM_cycles < 2) {
    	_motor->meas.previous_HFI_type = _motor->HFIType;
      uint16_t half_ARR = htim1.Instance->ARR / 2;
      htim1.Instance->CCR1 = half_ARR;
      htim1.Instance->CCR2 = half_ARR;
      htim1.Instance->CCR3 = half_ARR;
      _motor->m.R = 0.001f;     // Initialise with a very low value 1mR
      _motor->m.L_D = 0.000001f;  // Initialise with a very low value 1uH
      _motor->m.L_Q = 0.000001f;
      calculateVoltageGain(_motor);    // Set initial gains to enable MESCFOC to run
      calculateGains(_motor);
      phU_Enable(_motor);
      phV_Enable(_motor);
      phW_Enable(_motor);
      _motor->FOC.Idq_req.d = _motor->meas.measure_current;
      _motor->FOC.Idq_req.q = 0.0f;
      _motor->FOC.FOCAngle = 0;

      _motor->FOC.inject = 0;  // flag to not inject at SVPWM top

      MESCFOC(_motor);
      writePWM(_motor);

      _motor->meas.top_V = 0;
      _motor->meas.bottom_V = 0;
      _motor->meas.top_I = 0;
      _motor->meas.bottom_I = 0;
      _motor->meas.top_I_L = 0;
      _motor->meas.bottom_I_L = 0;
      _motor->meas.top_I_Lq = 0;
      _motor->meas.bottom_I_Lq = 0;

      _motor->meas.count_top = 0.0f;
      _motor->meas.count_bottom = 0.0f;
    }

    else if (_motor->meas.PWM_cycles < 35000) {  // Align the rotor for ~1 second
      _motor->FOC.Idq_req.d = _motor->meas.measure_current;
      _motor->FOC.Idq_req.q = 0.0f;

      _motor->FOC.inject = 0;
      MESCFOC(_motor);
      writePWM(_motor);
    }

    else if (_motor->meas.PWM_cycles < 40000) {  // Lower setpoint
      _motor->FOC.Idq_req.d = 0.20f*_motor->meas.measure_current;
      _motor->FOC.inject = 0;
      MESCFOC(_motor);
      writePWM(_motor);

      _motor->meas.bottom_V = _motor->meas.bottom_V + _motor->FOC.Vdq.d;
      _motor->meas.bottom_I = _motor->meas.bottom_I + _motor->FOC.Idq.d;
      _motor->meas.count_bottom++;
      _motor->meas.Vd_temp = _motor->FOC.Vdq.d * 1.0f;  // Store the voltage required for the low setpoint, to
                       	   	   	   	   	 // use as an offset for the inductance
    }

    else if (_motor->meas.PWM_cycles < 45000) {  // Upper setpoint stabilisation
      _motor->FOC.Idq_req.d = _motor->meas.measure_current;
      _motor->FOC.inject = 0;
      MESCFOC(_motor);
      writePWM(_motor);

    }

    else if (_motor->meas.PWM_cycles < 50000) {  // Upper setpoint
      _motor->FOC.Idq_req.d = _motor->meas.measure_current;
      _motor->FOC.inject = 0;
      MESCFOC(_motor);
      writePWM(_motor);

      _motor->meas.top_V = _motor->meas.top_V + _motor->FOC.Vdq.d;
      _motor->meas.top_I = _motor->meas.top_I + _motor->FOC.Idq.d;
      _motor->meas.count_top++;
    } else if (_motor->meas.PWM_cycles < 50001) {  // Calculate R

      generateBreak(_motor);
      _motor->m.R = (_motor->meas.top_V - _motor->meas.bottom_V) / (_motor->meas.top_I - _motor->meas.bottom_I);

      //Initialise the variables for the next measurement
      //Vd_temp = _motor->FOC.Vdq.d * 1.0f;  // Store the voltage required for the high setpoint, to
                       	   	   	   	   	 // use as an offset for the inductance
      _motor->meas.Vq_temp = 0.0f;
      _motor->FOC.Vdq.q = 0.0f;//
      _motor->FOC.Idq_int_err.d = 0.0f;
      _motor->FOC.Idq_int_err.q = 0.0f;
      _motor->meas.count_top = 0.0f;
      _motor->meas.count_bottom = 0.0f;
      _motor->meas.top_I_L = 0.0f;
      _motor->meas.bottom_I_L = 0.0f;

      generateEnable(_motor);
    }
/////////////////////////// Collect Ld variable//////////////////////////
    else if (_motor->meas.PWM_cycles < 80001) {
      // generateBreak();
	  _motor->HFIType = HFI_TYPE_SPECIAL;
      _motor->FOC.inject = 1;  // flag to the SVPWM writer to inject at top
      _motor->FOC.special_injectionVd = _motor->meas.measure_voltage;
      _motor->FOC.special_injectionVq = 0.0f;

      _motor->FOC.Vdq.d = _motor->meas.Vd_temp;
      _motor->FOC.Vdq.q = 0.0f;


      if (_motor->FOC.inject_high_low_now == 1) {
    	  _motor->meas.top_I_L = _motor->meas.top_I_L + _motor->FOC.Idq.d;
    	  _motor->meas.count_top++;
      } else if (_motor->FOC.inject_high_low_now == 0) {
    	  _motor->meas.bottom_I_L = _motor->meas.bottom_I_L + _motor->FOC.Idq.d;
    	  _motor->meas.count_bottom++;
      }
    }

    else if (_motor->meas.PWM_cycles < 80002) {
      generateBreak(_motor);
      _motor->m.L_D =
          fabsf((_motor->FOC.special_injectionVd) /
          ((_motor->meas.top_I_L - _motor->meas.bottom_I_L) / (_motor->meas.count_top * _motor->FOC.pwm_period)));
      _motor->meas.top_I_Lq = 0.0f;
      _motor->meas.bottom_I_Lq = 0.0f;
      _motor->meas.count_topq = 0.0f;
      _motor->meas.count_bottomq = 0.0f;
      __NOP();  // Put a break point on it...
    } else if (_motor->meas.PWM_cycles < 80003) {
      phU_Enable(_motor);
      phV_Enable(_motor);
      phW_Enable(_motor);

////////////////////////// Collect Lq variable//////////////////////////////
    } else if (_motor->meas.PWM_cycles < 100003) {
      //			generateBreak();
      _motor->FOC.special_injectionVd = 0.0f;
      _motor->FOC.special_injectionVq = _motor->meas.measure_voltage;
      _motor->FOC.inject = 1;  // flag to the SVPWM writer to update at top
      _motor->FOC.Vdq.d = _motor->meas.Vd_temp;  // Vd_temp to keep it aligned with D axis
      _motor->FOC.Vdq.q = 0.0f;


      if (_motor->FOC.inject_high_low_now == 1) {
    	  _motor->meas.top_I_Lq = _motor->meas.top_I_Lq + _motor->FOC.Idq.q;
    	  _motor->meas.count_topq++;
      } else if (_motor->FOC.inject_high_low_now == 0) {
    	  _motor->meas.bottom_I_Lq = _motor->meas.bottom_I_Lq + _motor->FOC.Idq.q;
        _motor->meas.count_bottomq++;
      }
    }

    else {
      generateBreak(_motor);
      _motor->HFIType = _motor->meas.previous_HFI_type;
      _motor->m.L_Q =
          fabsf((_motor->FOC.special_injectionVq) /
          ((_motor->meas.top_I_Lq - _motor->meas.bottom_I_Lq) / (_motor->meas.count_top * _motor->FOC.pwm_period)));

      _motor->MotorState = MOTOR_STATE_IDLE;

      _motor->FOC.inject = 0;  // flag to the SVPWM writer stop injecting at top
      _motor->FOC.special_injectionVd = 0.0f;
      _motor->FOC.special_injectionVq = 0.0f;
      _motor->FOC.Vd_injectionV = 0.0f;
      _motor->FOC.Vq_injectionV = 0.0f;
      calculateGains(_motor);
      _motor->MotorState = MOTOR_STATE_TRACKING;
      _motor->meas.PWM_cycles = 0;
      phU_Enable(_motor);
      phV_Enable(_motor);
      phW_Enable(_motor);
    }
    _motor->meas.PWM_cycles++;
  }


  void getHallTable(MESC_motor_typedef *_motor) {
    static int firstturn = 1;
    static int hallstate;
    hallstate = getHallState();
    static int lasthallstate = -1;
    static uint16_t pwm_count = 0;
    static int anglestep = 5;  // This defines how fast the motor spins
    static uint32_t hallangles[7][2];
    static int rollover;
    hallstate = _motor->hall.current_hall_state;
    if (firstturn) {
    	generateEnable(_motor);
      lasthallstate = hallstate;
      (void)lasthallstate;
      firstturn = 0;
    }

    ////// Align the rotor////////////////////
    static uint16_t a = 65535;
    if (a)  // Align time
    {
      _motor->FOC.Idq_req.d = 10.0f;
      _motor->FOC.Idq_req.q = 0.0f;

      _motor->FOC.FOCAngle = 0.0f;
      a = a - 1;
    } else {
      _motor->FOC.Idq_req.d = 10.0f;
      _motor->FOC.Idq_req.q = 0.0f;
      static int dir = 1;
      if (pwm_count < 65534) {
        if (_motor->FOC.FOCAngle < (anglestep)) {
          rollover = hallstate;
        }
        if ((_motor->FOC.FOCAngle < (30000)) &&
            (_motor->FOC.FOCAngle > (29000 - anglestep))) {
          rollover = 0;
        }
        lasthallstate = hallstate;
        if (rollover == hallstate) {
          hallangles[hallstate][0] =
              hallangles[hallstate][0] +
              (uint32_t)65535;  // Accumulate the angles through the sweep
        }

        _motor->FOC.FOCAngle =
            _motor->FOC.FOCAngle + anglestep;  // Increment the angle
        hallangles[hallstate][0] =
            hallangles[hallstate][0] +
            _motor->FOC.FOCAngle;       // Accumulate the angles through the sweep
        hallangles[hallstate][1]++;  // Accumulate the number of PWM pulses for
                                     // this hall state
        pwm_count = pwm_count + 1;
      } else if (pwm_count < 65535) {
        if (dir == 1) {
          dir = 0;
          rollover = 0;
        }
        if ((_motor->FOC.FOCAngle < (12000)) && (hallstate != _motor->hall.last_hall_state)) {
          rollover = hallstate;
        }
        if ((_motor->FOC.FOCAngle < (65535)) &&
            (_motor->FOC.FOCAngle > (65535 - anglestep))) {
          rollover = 0;
        }
        lasthallstate = hallstate;
        if (rollover == hallstate) {
          hallangles[hallstate][0] =
              hallangles[hallstate][0] +
              (uint32_t)65535;  // Accumulate the angles through the sweep
        }

        _motor->FOC.FOCAngle =
            _motor->FOC.FOCAngle - anglestep;  // Increment the angle
        hallangles[hallstate][0] =
            hallangles[hallstate][0] +
            _motor->FOC.FOCAngle;       // Accumulate the angles through the sweep
        hallangles[hallstate][1]++;  // Accumulate the number of PWM pulses for
                                     // this hall state
        pwm_count = pwm_count + 1;
      }
    }
    if (pwm_count == 65535) {
      generateBreak(_motor);  // Debugging
      for (int i = 1; i < 7; i++) {
        hallangles[i][0] = hallangles[i][0] / hallangles[i][1];
        if (hallangles[i][0] > 65535) {
          hallangles[i][0] = hallangles[i][0] - 65535;
        }
      }
      for (int i = 0; i < 6; i++) {
            _motor->m.hall_table[i][2] = hallangles[i + 1][0];//This is the center angle of the hall state
            _motor->m.hall_table[i][3] = hallangles[i + 1][1];//This is the width of the hall state
            _motor->m.hall_table[i][0] = _motor->m.hall_table[i][2]-_motor->m.hall_table[i][3]/2;//This is the start angle of the hall state
            _motor->m.hall_table[i][1] = _motor->m.hall_table[i][2]+_motor->m.hall_table[i][3]/2;//This is the end angle of the hall state
      }
      _motor->MotorState = MOTOR_STATE_TRACKING;
      _motor->FOC.Idq_req.d = 0;
      _motor->FOC.Idq_req.q = 0;
      phU_Enable(_motor);
      phV_Enable(_motor);
      phW_Enable(_motor);
    }
  }

  void measureInductance(MESC_motor_typedef *_motor){  // UNUSED, THIS HAS BEEN ROLLED INTO THE MEASURE
                            // RESISTANCE... no point in 2 functions really...
__NOP();
  }


  void getkV(MESC_motor_typedef *_motor) {
	_motor->meas.previous_HFI_type = _motor->HFIType;
	_motor->HFIType=HFI_TYPE_NONE;
  	_motor->FOC.inject = 0;

    static int cycles = 0;
    static HFI_type_e old_HFI_type;
    if (cycles < 2) {
    	_motor->m.flux_linkage_max = 0.1f;
    	_motor->m.flux_linkage_min = 0.00001f;//Set really wide limits
    	_motor->FOC.openloop_step = 0;
    	_motor->FOC.flux_observed = _motor->m.flux_linkage_max;
    	old_HFI_type = _motor->HFIType;
    	_motor->HFIType = HFI_TYPE_NONE;
        phU_Enable(_motor);
        phV_Enable(_motor);
        phW_Enable(_motor);
    }

    flux_observer(_motor);//We run the flux observer during this

    static int count = 0;
    static uint16_t temp_angle;
    if (cycles < 60002) {
        _motor->FOC.Idq_req.d = _motor->meas.measure_current*0.5f;  //
        _motor->FOC.Idq_req.q = 0.0f;
    	_motor->meas.angle_delta = temp_angle-_motor->FOC.FOCAngle;
    	_motor->FOC.openloop_step = (uint16_t)(ERPM_MEASURE*65536.0f/(_motor->FOC.pwm_frequency*60.0f)*(float)cycles/65000.0f);
    	_motor->FOC.FOCAngle = temp_angle;
        OLGenerateAngle(_motor);
        temp_angle = _motor->FOC.FOCAngle;
        if(cycles==60001){
        	_motor->meas.temp_flux = sqrtf(_motor->FOC.Vdq.d*_motor->FOC.Vdq.d+_motor->FOC.Vdq.q*_motor->FOC.Vdq.q)/(6.28f * (float)_motor->FOC.openloop_step * (float)_motor->FOC.pwm_frequency/65536.0f);
        	_motor->FOC.flux_observed  = _motor->meas.temp_flux;
        	_motor->FOC.flux_a = _motor->FOC.sincosangle.cos*_motor->FOC.flux_observed;
        	_motor->FOC.flux_b = _motor->FOC.sincosangle.sin*_motor->FOC.flux_observed;
        	_motor->m.flux_linkage_max = 1.7f*_motor->FOC.flux_observed;
        	_motor->m.flux_linkage_min = 0.5f*_motor->FOC.flux_observed;
        	_motor->meas.temp_FLA = _motor->FOC.flux_a;
        	_motor->meas.temp_FLB = _motor->FOC.flux_b;
        }
        MESCFOC(_motor);
    }
    else if (cycles < 61000) {
    	generateBreak(_motor);
    	ADCPhaseConversion(_motor);
    	MESCTrack(_motor);
    }
    else if (cycles < 70001) {
    	generateEnable(_motor);
        _motor->FOC.Idq_int_err.d = 0.0f;
        MESCFOC(_motor);

      count++;
      _motor->FOC.Idq_req.d = 0.0f;
      _motor->FOC.Idq_req.q = IMEASURE_CLOSEDLOOP;
    }
    else if (cycles < 128000) {
      count++;
      _motor->FOC.Idq_req.d = 0.0f;
      _motor->FOC.Idq_req.q = IMEASURE_CLOSEDLOOP;
      MESCFOC(_motor);
    } else {
       generateBreak(_motor);
       _motor->m.flux_linkage = _motor->FOC.flux_observed;
       calculateFlux(_motor);
      _motor->MotorState = MOTOR_STATE_TRACKING;
      _motor->HFIType = old_HFI_type;
      cycles = 0;
      _motor->HFIType = _motor->meas.previous_HFI_type;
      if (_motor->m.flux_linkage > 0.0001f && _motor->m.flux_linkage < 200.0f) {
    	_motor->MotorSensorMode = MOTOR_SENSOR_MODE_SENSORLESS;
      } else {
        _motor->MotorState = MOTOR_STATE_ERROR;
        generateBreak(_motor);
      }
    }
    writePWM(_motor);

    cycles++;

  }

  uint32_t tmpccmrx;  // Temporary buffer which is used to turn on/off phase PWMs

  // Turn all phase U FETs off, Tristate the HBridge output - For BLDC mode
  // mainly, but also used for measuring, software fault detection and recovery
  void phU_Break(MESC_motor_typedef *_motor) {
    tmpccmrx = htim1.Instance->CCMR1;
    tmpccmrx &= ~TIM_CCMR1_OC1M;
    tmpccmrx &= ~TIM_CCMR1_CC1S;
    tmpccmrx |= TIM_OCMODE_FORCED_INACTIVE;
    htim1.Instance->CCMR1 = tmpccmrx;
    htim1.Instance->CCER &= ~TIM_CCER_CC1E;   // disable
    htim1.Instance->CCER &= ~TIM_CCER_CC1NE;  // disable
  }
  // Basically un-break phase U, opposite of above...
  void phU_Enable(MESC_motor_typedef *_motor) {
    tmpccmrx = htim1.Instance->CCMR1;
    tmpccmrx &= ~TIM_CCMR1_OC1M;
    tmpccmrx &= ~TIM_CCMR1_CC1S;
    tmpccmrx |= TIM_OCMODE_PWM1;
    htim1.Instance->CCMR1 = tmpccmrx;
    htim1.Instance->CCER |= TIM_CCER_CC1E;   // enable
    htim1.Instance->CCER |= TIM_CCER_CC1NE;  // enable
  }

  void phV_Break(MESC_motor_typedef *_motor) {
    tmpccmrx = htim1.Instance->CCMR1;
    tmpccmrx &= ~TIM_CCMR1_OC2M;
    tmpccmrx &= ~TIM_CCMR1_CC2S;
    tmpccmrx |= TIM_OCMODE_FORCED_INACTIVE << 8;
    htim1.Instance->CCMR1 = tmpccmrx;
    htim1.Instance->CCER &= ~TIM_CCER_CC2E;   // disable
    htim1.Instance->CCER &= ~TIM_CCER_CC2NE;  // disable
  }

  void phV_Enable(MESC_motor_typedef *_motor) {
    tmpccmrx = htim1.Instance->CCMR1;
    tmpccmrx &= ~TIM_CCMR1_OC2M;
    tmpccmrx &= ~TIM_CCMR1_CC2S;
    tmpccmrx |= TIM_OCMODE_PWM1 << 8;
    htim1.Instance->CCMR1 = tmpccmrx;
    htim1.Instance->CCER |= TIM_CCER_CC2E;   // enable
    htim1.Instance->CCER |= TIM_CCER_CC2NE;  // enable
  }

  void phW_Break(MESC_motor_typedef *_motor) {
    tmpccmrx = htim1.Instance->CCMR2;
    tmpccmrx &= ~TIM_CCMR2_OC3M;
    tmpccmrx &= ~TIM_CCMR2_CC3S;
    tmpccmrx |= TIM_OCMODE_FORCED_INACTIVE;
    htim1.Instance->CCMR2 = tmpccmrx;
    htim1.Instance->CCER &= ~TIM_CCER_CC3E;   // disable
    htim1.Instance->CCER &= ~TIM_CCER_CC3NE;  // disable
  }

  void phW_Enable(MESC_motor_typedef *_motor) {
    tmpccmrx = htim1.Instance->CCMR2;
    tmpccmrx &= ~TIM_CCMR2_OC3M;
    tmpccmrx &= ~TIM_CCMR2_CC3S;
    tmpccmrx |= TIM_OCMODE_PWM1;
    htim1.Instance->CCMR2 = tmpccmrx;
    htim1.Instance->CCER |= TIM_CCER_CC3E;   // enable
    htim1.Instance->CCER |= TIM_CCER_CC3NE;  // enable
  }

  void calculateFlux(MESC_motor_typedef *_motor) {
	  _motor->m.flux_linkage_max = 1.7f*_motor->m.flux_linkage;
	  _motor->m.flux_linkage_min = 0.5f*_motor->m.flux_linkage;
	  _motor->m.flux_linkage_gain = 10.0f * sqrtf(_motor->m.flux_linkage);
	  _motor->m.non_linear_centering_gain = NON_LINEAR_CENTERING_GAIN;
  }

  void calculateGains(MESC_motor_typedef *_motor) {
    _motor->FOC.pwm_period = 1.0f/_motor->FOC.pwm_frequency;
    _motor->mtimer->Instance->ARR = HAL_RCC_GetHCLKFreq()/(((float)htim1.Instance->PSC + 1.0f) * 2*_motor->FOC.pwm_frequency);
    _motor->mtimer->Instance->CCR4 = htim1.Instance->ARR-50; //Just short of dead center (dead center will not actually trigger the conversion)
    #ifdef SINGLE_ADC
    _motor->mtimer->Instance->CCR4 = htim1.Instance->ARR-80; //If we only have one ADC, we need to convert early otherwise the data will not be ready in time
    #endif
    _motor->FOC.PWMmid = htim1.Instance->ARR * 0.5f;

    _motor->FOC.ADC_duty_threshold = htim1.Instance->ARR * 0.85f;


    calculateFlux(_motor);


    _motor->FOC.Current_bandwidth = CURRENT_BANDWIDTH;
    //PID controller gains
    _motor->FOC.Id_pgain = _motor->FOC.Current_bandwidth * _motor->m.L_D;
    _motor->FOC.Id_igain = _motor->m.R / _motor->m.L_D;
    // Pole zero cancellation for series PI control
    _motor->FOC.Iq_pgain = _motor->FOC.Id_pgain;
    _motor->FOC.Iq_igain = _motor->FOC.Id_igain;

    _motor->FOC.field_weakening_curr_max = FIELD_WEAKENING_CURRENT;  // test number, to be stored in user settings
  _motor->m.L_QD = _motor->m.L_Q-_motor->m.L_D;
  _motor->FOC.d_polarity = 1;

  }

  void calculateVoltageGain(MESC_motor_typedef *_motor) {
    // We need a number to convert between Va Vb and raw PWM register values
    // This number should be the bus voltage divided by the ARR register
    _motor->FOC.Vab_to_PWM =
        htim1.Instance->ARR / _motor->Conv.Vbus;
    // We also need a number to set the maximum voltage that can be effectively
    // used by the SVPWM This is equal to
    // 0.5*Vbus*MAX_MODULATION*SVPWM_MULTIPLIER*Vd_MAX_PROPORTION
    _motor->FOC.Vmag_max = 0.5f * _motor->Conv.Vbus *
            MAX_MODULATION * SVPWM_MULTIPLIER;
    _motor->FOC.Vmag_max2 = _motor->FOC.Vmag_max*_motor->FOC.Vmag_max;
    _motor->FOC.Vd_max = 0.5f * _motor->Conv.Vbus *
                      MAX_MODULATION * SVPWM_MULTIPLIER * Vd_MAX_PROPORTION;
    _motor->FOC.Vq_max = 0.5f * _motor->Conv.Vbus *
                      MAX_MODULATION * SVPWM_MULTIPLIER * Vq_MAX_PROPORTION;
#ifdef USE_SQRT_CIRCLE_LIM
    _motor->FOC.Vd_max = _motor->FOC.Vmag_max;
    _motor->FOC.Vq_max = _motor->FOC.Vmag_max;
#endif

    _motor->FOC.Vdint_max = _motor->FOC.Vd_max * 0.9f; //Logic in this is to always ensure headroom for the P term
    _motor->FOC.Vqint_max = _motor->FOC.Vq_max * 0.9f;

    _motor->FOC.field_weakening_threshold = _motor->FOC.Vq_max * FIELD_WEAKENING_THRESHOLD;
    _motor->FOC.field_weakening_multiplier = 1.0f/(_motor->FOC.Vq_max*(1.0f-FIELD_WEAKENING_THRESHOLD));

    switch(_motor->HFIType){//When running HFI we want the bandwidth low, so we calculate it with each slow loop depending on whether we are HFIing or not

    case HFI_TYPE_45:
    	//fallthrough
    case HFI_TYPE_D:
    	//fallthrough
    case HFI_TYPE_SPECIAL:

		_motor->FOC.Id_pgain = _motor->FOC.Current_bandwidth * _motor->m.L_D;
		_motor->FOC.Id_igain = _motor->m.R / _motor->m.L_D;
		// Pole zero cancellation for series PI control
		_motor->FOC.Iq_pgain = _motor->FOC.Id_pgain;
		_motor->FOC.Iq_igain = _motor->FOC.Id_igain;
		//This is the expected current magnitude we would see based on the average inductance and the injected voltage. Not particularly reliable currently.
		//_motor->FOC.HFI_Threshold = ((HFI_VOLTAGE*sqrt2*2.0f)*_motor->FOC.pwm_period)/((_motor->m.L_D+_motor->m.L_Q)*0.5f);
		if(HFI_THRESHOLD==0.0f){
		_motor->FOC.HFI_toggle_voltage = mtr->Conv.Vbus*0.05f;
			if(_motor->FOC.HFI_toggle_voltage<3.0f){_motor->FOC.HFI_toggle_voltage = 3.0f;}
		}else{
		_motor->FOC.HFI_toggle_voltage = HFI_THRESHOLD;
		}
		break;
    case HFI_TYPE_NONE:
    	__NOP();
    	break;
    }
  }


  static int dp_periods = 3;
  void doublePulseTest(MESC_motor_typedef *_motor) {
    static int dp_counter;
    if  (dp_counter == 0) { //Let bootstrap charge
        phU_Enable(_motor);
        phV_Enable(_motor);
        phW_Enable(_motor);
        htim1.Instance->CCR1 = 0;
        htim1.Instance->CCR2 = 0;
        htim1.Instance->CCR3 = 0;
        test_vals.dp_current_final[dp_counter] =
            _motor->Conv.Iv;
        dp_counter++;
      } else if(dp_counter <= (dp_periods-2)) { //W State ON
      htim1.Instance->CCR1 = 0;
      htim1.Instance->CCR2 = 0;
      htim1.Instance->CCR3 = htim1.Instance->ARR;
      phU_Break(_motor);
      phV_Enable(_motor);
      phW_Enable(_motor);
      test_vals.dp_current_final[dp_counter] =
          _motor->Conv.Iv;
      dp_counter++;
    } else if (dp_counter == (dp_periods-1)) { //W short second pulse
        htim1.Instance->CCR2 = 0;
        htim1.Instance->CCR3 = 100;
        test_vals.dp_current_final[dp_counter] =
            _motor->Conv.Iv;
        dp_counter++;
     } else if (dp_counter == dp_periods) { //Freewheel a bit to see the current
          htim1.Instance->CCR2 = 0;
          htim1.Instance->CCR3 = 0;
          test_vals.dp_current_final[dp_counter] =
              _motor->Conv.Iv;
          dp_counter++;
        }else { //Turn all off
      htim1.Instance->CCR1 = 0;
      htim1.Instance->CCR2 = 0;
      htim1.Instance->CCR3 = 0;
      test_vals.dp_current_final[dp_counter] =
          _motor->Conv.Iv;
      dp_counter = 0;
      generateBreak(_motor);
      _motor->MotorState = MOTOR_STATE_IDLE;
    }
  }
  void MESC_Slow_IRQ_handler(MESC_motor_typedef *_motor){


	  if(_motor->stimer->Instance->SR & TIM_FLAG_CC2){
		  input_vars.IC_duration = _motor->stimer->Instance->CCR1;// HAL_TIM_ReadCapturedValue(&htim4 /*&htim3*/, TIM_CHANNEL_1);
		  input_vars.IC_pulse = _motor->stimer->Instance->CCR2;//HAL_TIM_ReadCapturedValue(&htim4 /*&htim3*/, TIM_CHANNEL_2);
		  input_vars.pulse_recieved = 1;

	  }else{
		  input_vars.IC_duration = 50000;
		  input_vars.IC_pulse = 0;
		  input_vars.pulse_recieved = 0;

	  }

	    if(_motor->stimer->Instance->SR & TIM_FLAG_UPDATE){
	    		      slowLoop(_motor);
	    }
  }
  extern uint32_t ADC_buffer[6];

float  Square(float x){ return((x)*(x));}

  void slowLoop(MESC_motor_typedef *_motor) {
    // In this loop, we will fetch the throttle values, and run functions that
    // are critical, but do not need to be executed very often e.g. adjustment
    // for battery voltage change

	  houseKeeping(_motor);	//General dross that keeps things ticking over, like nudging the observer
	  switch(_motor->ControlMode){
		  case MOTOR_CONTROL_MODE_TORQUE:
			  collectInputs(_motor); //Get all the throttle inputs
			  _motor->FOC.Idq_req.q = input_vars.Idq_req_UART.q + input_vars.Idq_req_RCPWM.q + input_vars.Idq_req_ADC1.q + input_vars.Idq_req_ADC2.q;
			  //Clamp the Q component; d component is not directly requested
				if(_motor->FOC.Idq_req.q>input_vars.max_request_Idq.q){_motor->FOC.Idq_req.q = input_vars.max_request_Idq.q;}
				if(_motor->FOC.Idq_req.q<input_vars.min_request_Idq.q){_motor->FOC.Idq_req.q = input_vars.min_request_Idq.q;}
			break;
		  case MOTOR_CONTROL_MODE_POSITION:
			  //TBC, needs some kind of velocity generation curve to nest into the speed loop
			  //fallthrough, once the position controller has generated a speed it probably wants to go straight to the speed controller
		  case MOTOR_CONTROL_MODE_SPEED:
			  //TBC PID loop to convert eHz feedback to an iq request
			  break;
		  case MOTOR_CONTROL_MODE_DUTY:
			  //TBC, need to adjust the max modulation index to allow it to bounce off the circle limiter
			  break;

		  default:
			  __NOP();
			  break;
	  }
		///////////////////////Run the state machine//////////////////////////////////
	switch(_motor->MotorState){
		case MOTOR_STATE_TRACKING:
			_motor->FOC.was_last_tracking = 1;
			if(fabsf(_motor->FOC.Idq_req.q)>0.2f){
				#ifdef HAS_PHASE_SENSORS
				if(_motor->MotorControlType == MOTOR_CONTROL_TYPE_FOC){
				_motor->MotorState = MOTOR_STATE_RUN;
				}else if(_motor->MotorControlType == MOTOR_CONTROL_TYPE_BLDC){
					_motor->MotorState = MOTOR_STATE_RUN_BLDC;

				}
				#else
				_motor->MotorState = MOTOR_STATE_RECOVERING;
				break;
				#endif
				//fallthrough
			}else{
				//Remain in tracking
				break;
			}

		case MOTOR_STATE_RUN:
			calculatePower(_motor);
			ThrottleTemperature(_motor); //Gradually ramp down the Q current if motor or FETs are getting hot
			RunMTPA(_motor);//Process MTPA
			LimitFWCurrent(_motor);//Process FW -> Iq reduction
			clampBatteryPower(_motor); //Prevent too much power being drawn from the battery
			#ifdef USE_LR_OBSERVER
			LRObserver();
			#endif
			if((fabsf(_motor->FOC.Idq_req.q)<0.1f)){//Request current small, FW not active
				if((_motor->FOC.FW_current>-0.5f)){
				_motor->MotorState = MOTOR_STATE_TRACKING;
				}else{
					FWRampDown(_motor);
				}
			}
			generateEnable(_motor);
			SlowHFI(_motor);
			break;
		case MOTOR_STATE_RUN_BLDC:
			_motor->BLDC.I_set = _motor->FOC.Idq_req.q;
			break;

		case MOTOR_STATE_ERROR:
				//add recovery stuff
			if(fabsf(_motor->FOC.Idq_req.q)<0.1f){
				_motor->MotorState = MOTOR_STATE_TRACKING;
				VICheck(_motor); //Immediately return it to error state if there is still a critical fault condition active
			}
			break;
		case MOTOR_STATE_SLAMBRAKE:
			__NOP();
			//We might want to do something if there is a handbrake state? Like exiting this state?
			break;
		default:
			__NOP();
			//This accounts for all the initialising, test, measuring... procedures.
			//We basically just want to do nothing and let them get on with their job.
			break;
	}
	/////////////////End of Switch state machine///////////////////////////////

	calculateVoltageGain(_motor);
}


  void MESCTrack(MESC_motor_typedef *_motor) {
    // here we are going to do the clark and park transform of the voltages to
    // get the VaVb and VdVq These can be handed later to the observers and used
    // to set the integral terms

    // Clark transform
    _motor->FOC.Vab.a =
        0.666f * (_motor->Conv.Vu -
                  0.5f * ((_motor->Conv.Vv) +
                          (_motor->Conv.Vw)));
    _motor->FOC.Vab.b =
        0.666f *
        (sqrt3_on_2 * ((_motor->Conv.Vv) -
                       (_motor->Conv.Vw)));

    sin_cos_fast(_motor->FOC.FOCAngle, &_motor->FOC.sincosangle.sin, &_motor->FOC.sincosangle.cos);

    // Park transform

    _motor->FOC.Vdq.d = _motor->FOC.sincosangle.cos * _motor->FOC.Vab.a +
                      _motor->FOC.sincosangle.sin * _motor->FOC.Vab.b;
    _motor->FOC.Vdq.q = _motor->FOC.sincosangle.cos * _motor->FOC.Vab.b -
                      _motor->FOC.sincosangle.sin * _motor->FOC.Vab.a;
    _motor->FOC.Idq_int_err.q = _motor->FOC.Vdq.q;
  }


  float IacalcDS, IbcalcDS, VacalcDS, VbcalcDS, VdcalcDS, VqcalcDS, FLaDS, FLbDS, FLaDSErr, FLbDSErr;
  uint16_t angleDS, angleErrorDSENC, angleErrorPhaseSENC, angleErrorPhaseDS, countdown_cycles;

  void deadshort(MESC_motor_typedef *_motor){
	  // LICENCE NOTE:
	  // This function deviates slightly from the BSD 3 clause licence.
	  // The work here is entirely original to the MESC FOC project, and not based
	  // on any appnotes, or borrowed from another project. This work is free to
	  // use, as granted in BSD 3 clause, with the exception that this note must
	  // be included in where this code is implemented/modified to use your
	  // variable names, structures containing variables or other minor
	  // rearrangements in place of the original names I have chosen, and credit
	  // to David Molony as the original author must be noted.

	  //This "deadshort " function is an original idea (who knows, someone may have had it before) for finding the rotor angle
	  //Concept is that when starting from spinning with no phase sensors or encoder, you need to know the angle and the voltages.
	  //To achieve this, we simply short out the motor for a PWM period and allow the current to build up.
	  //We can then calculate the voltage from V=Ldi/dt in the alpha beta reference frame
	  //We can calculate the angle from the atan2 of the alpha beta voltages
	  //With this angle, we can get Vd and Vq for preloading the PI controllers
	  //We can also preload the flux observer with motor.motorflux*sin and motor.motorflux*cos terms

	static uint16_t countdown = 10;

	  		if(countdown == 1||(((_motor->FOC.Iab.a*_motor->FOC.Iab.a+_motor->FOC.Iab.b*_motor->FOC.Iab.b)>DEADSHORT_CURRENT*DEADSHORT_CURRENT)&&countdown<9))
	  				{
	  					//Need to collect the ADC currents here
	  					generateBreak(_motor);
	  					//Calculate the voltages in the alpha beta phase...
	  					IacalcDS = _motor->FOC.Iab.a;
	  					IbcalcDS = _motor->FOC.Iab.b;
	  					VacalcDS = -_motor->m.L_D*_motor->FOC.Iab.a/((9.0f-(float)countdown)*_motor->FOC.pwm_period);
	  					VbcalcDS = -_motor->m.L_D*_motor->FOC.Iab.b/((9.0f-(float)countdown)*_motor->FOC.pwm_period);
	  					//Calculate the phase angle
	  					//TEST LINE angleDS = (uint16_t)(32768.0f + 10430.0f * fast_atan2(VbcalcDS, VacalcDS)) - 32768;// +16384;

	  					 angleDS = (uint16_t)(32768.0f + 10430.0f * fast_atan2(VbcalcDS, VacalcDS)) - 32768 -16384;
	  					//Shifting by 1/4 erev does not work for going backwards. Need to rethink.
	  					//Problem is, depending on motor direction, the sign of the voltage generated swaps for the same rotor position.
	  					//The atan2(flux linkages) is stable under this regime, but the same for voltage is not.
	  					_motor->FOC.FOCAngle = angleDS;//_motor->FOC.enc_angle;//
	  					sin_cos_fast(_motor->FOC.FOCAngle, &_motor->FOC.sincosangle.sin, &_motor->FOC.sincosangle.cos);

	  					//Park transform it to get VdVq
	  					VdcalcDS = _motor->FOC.sincosangle.cos * VacalcDS +
	  				                      _motor->FOC.sincosangle.sin * VbcalcDS;
	  					VqcalcDS = _motor->FOC.sincosangle.cos * VbcalcDS -
	  				                      _motor->FOC.sincosangle.sin * VacalcDS;
	  					//Preloading the observer
	  					FLaDS = _motor->FOC.flux_observed*_motor->FOC.sincosangle.cos;
	  					FLbDS = _motor->FOC.flux_observed*_motor->FOC.sincosangle.sin;
	  		//Angle Errors for debugging
	  					angleErrorDSENC = angleDS-_motor->FOC.enc_angle;
	  		//			angleErrorPhaseSENC = _motor->FOC.FOCAngle-_motor->FOC.enc_angle;
	  		//			angleErrorPhaseDS = _motor->FOC.FOCAngle - angleDS;
	  		//Variables for monitoring and debugging to see if the preload will work
	  		//			FLaDSErr = 1000.0f*(FLaDS-_motor->FOC.flux_a);
	  		//			FLbDSErr = 1000.0f*(FLbDS-_motor->FOC.flux_b);

	  		//Do actual preloading
	  					_motor->FOC.flux_a = FLaDS;
	  					_motor->FOC.flux_b = FLbDS;
	  					_motor->FOC.Ia_last = 0.0f;
	  					_motor->FOC.Ib_last = 0.0f;
	  					_motor->FOC.Idq_int_err.d = VdcalcDS;
	  					_motor->FOC.Idq_int_err.q = VqcalcDS;
	  		//Next PWM cycle it  will jump to running state,
	  					MESCFOC(_motor);
	  					countdown_cycles = 9-countdown;
	  					countdown = 1;



	  		}
	  		if(countdown > 10){
	  			generateBreak(_motor);
	  			htim1.Instance->CCR1 = 50;
	  			htim1.Instance->CCR2 = 50;
	  			htim1.Instance->CCR3 = 50;
	  			//Preload the timer at mid
	  		}
	  		if(countdown <= 10 && countdown>1 ){
	  			htim1.Instance->CCR1 = 50;
	  			htim1.Instance->CCR2 = 50;
	  			htim1.Instance->CCR3 = 50;
	  			generateEnable(_motor);
	  		}
	  		if(countdown == 1 ){
					countdown = 15; //We need at least a few cycles for the current to relax
									//to zero in case of rapid switching between states
  					_motor->MotorState = MOTOR_STATE_RUN;

	  		}
	  		countdown--;
  }

  uint8_t pkt_crc8(uint8_t crc/*CRC_SEED=0xFF*/, uint8_t *data, uint8_t length)
  {
      int16_t i, bit;

      for (i = 0; i < length; i++)
      {
          crc ^= data[i];

          for (bit = 0; bit < 8; bit++)
          {
              if ((crc & 0x80) != 0)
              {
                  crc <<= 1;
                  crc ^= 0x1D; //CRC_POLYNOMIAL=0x1D;
              }
              else
              {
                  crc <<= 1;
              }
          }
      }

      return crc;
  }

  struct __attribute__ ((__packed__))SamplePacket
  {
	  	struct
	  	{
	  		uint8_t crc;
	  		uint8_t STAT_RESP; // Should be 0xF_?
	  	}safetyword;
  	uint16_t angle;
  	int16_t speed;
  	uint16_t revolutions;
  };

  typedef struct SamplePacket SamplePacket;
	  SamplePacket pkt;

  void tle5012(MESC_motor_typedef *_motor)
  {
#ifdef USE_ENCODER
	  uint16_t const len = sizeof(pkt) / sizeof(uint16_t);
	  uint16_t reg = (UINT16_C(  1) << 15) /* RW=Read */
	               | (UINT16_C(0x0) << 11) /* Lock */
	               | (UINT16_C(0x0) << 10) /* UPD=Buffer */
	               | (UINT16_C(0x02) << 4) /* ADDR */
	               | (len -1);            /* ND */
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
      HAL_SPI_Transmit( &hspi3, (uint8_t *)&reg,   1, 1000 );
      HAL_SPI_Receive(  &hspi3, (uint8_t *)&pkt, len, 1000 );
//      volatile uint8_t crc = 0;
//#if 1
//      reg ^= 0xFF00;
//      crc = pkt_crc8( crc, &((uint8_t *)&reg)[1], 1 );
//      crc = pkt_crc8( crc, &((uint8_t *)&reg)[0], 1 );
//      crc = pkt_crc8( crc, &((uint8_t *)&pkt.angle)[1], 1 );
//      crc = pkt_crc8( crc, &((uint8_t *)&pkt.angle)[0], 1 );
//      crc = pkt_crc8( crc, &((uint8_t *)&pkt.speed)[1], 1 );
//      crc = pkt_crc8( crc, &((uint8_t *)&pkt.speed)[0], 1 );
//      crc = pkt_crc8( crc, &((uint8_t *)&pkt.revolutions)[1], 1 );
//      crc = pkt_crc8( crc, &((uint8_t *)&pkt.revolutions)[0], 1 );
//#else
//      crc = pkt_crc8( crc, &reg, 2 );
//      crc = pkt_crc8( crc, &pkt.angle, 6 );
//#endif
//      crc = pkt_crc8( crc, &pkt.safetyword.STAT_RESP, 1 );
//      crc = ~crc;
//      if (crc != pkt.safetyword.crc)
//      {
//    	  __NOP();
//    	  __NOP();
//    	  __NOP();
//      }
//      else
//      {
//    	  __NOP();
//      }

      pkt.angle = pkt.angle & 0x7fff;
#ifdef ENCODER_DIR_REVERSED
      	  _motor->FOC.enc_angle = -POLE_PAIRS*((pkt.angle *2)%POLE_ANGLE)-_motor->FOC.enc_offset;
#else
      _motor->FOC.enc_angle = POLE_PAIRS*((pkt.angle *2)%POLE_ANGLE)-_motor->FOC.enc_offset;
#endif
      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
      pkt.revolutions = pkt.revolutions&0b0000000111111111;
#endif
  }


uint16_t test_on_time;
uint16_t test_on_time_acc[3];
uint16_t test_counts;
  void getDeadtime(MESC_motor_typedef *_motor){
	  static int use_phase = 0;

	  if(test_on_time<1){test_on_time = 1;}

		if(use_phase==0){
			htim1.Instance->CCR1 = test_on_time;
			htim1.Instance->CCR2 = 0;
			htim1.Instance->CCR3 = 0;
			if(_motor->Conv.Iu<1.0f){ test_on_time=test_on_time+1;}
			if(_motor->Conv.Iu>1.0f){ test_on_time=test_on_time-1;}
			generateEnable(_motor);
			test_on_time_acc[0] = test_on_time_acc[0]+test_on_time;
			}
		if(use_phase==1){
			htim1.Instance->CCR1 = 0;
			htim1.Instance->CCR2 = test_on_time;
			htim1.Instance->CCR3 = 0;
			if(_motor->Conv.Iv<1.0f){ test_on_time=test_on_time+1;}
			if(_motor->Conv.Iv>1.0f){ test_on_time=test_on_time-1;}
			generateEnable(_motor);
			test_on_time_acc[1] = test_on_time_acc[1]+test_on_time;
		}
		if(use_phase==2){
			htim1.Instance->CCR1 = 0;
			htim1.Instance->CCR2 = 0;
			htim1.Instance->CCR3 = test_on_time;
			if(_motor->Conv.Iw<1.0f){ test_on_time=test_on_time+1;}
			if(_motor->Conv.Iw>1.0f){ test_on_time=test_on_time-1;}
			generateEnable(_motor);
			test_on_time_acc[2] = test_on_time_acc[2]+test_on_time;
		}
		if(use_phase>2){
			generateBreak(_motor);
			_motor->MotorState = MOTOR_STATE_TRACKING;
			use_phase = 0;
			test_on_time_acc[0] = test_on_time_acc[0]>>10;
			test_on_time_acc[1] = test_on_time_acc[1]>>10;
			test_on_time_acc[2] = test_on_time_acc[2]>>10;
			deadtime_comp = test_on_time_acc[0];
		}
		test_counts++;

	if(test_counts>511){
		use_phase++;
	test_counts = 0;
	}
  }

  //LR observer. WIP function that works by injecting a
  //low frequency Id signal into the PID input and observing the change in Vd and Vq
  //Does not work too well, requires some care in use.
  //Original work to MESC project.
  float Vd_obs_high, Vd_obs_low, R_observer, Vq_obs_high, Vq_obs_low, L_observer, Last_eHz, LR_collect_count;
  float Vd_obs_high_filt, Vd_obs_low_filt,Vq_obs_high_filt,Vq_obs_low_filt;
  static int plusminus = 1;

  void LRObserver(MESC_motor_typedef *_motor){
	  if((fabsf(_motor->FOC.eHz)>0.005f*_motor->FOC.pwm_frequency)&&(_motor->FOC.inject ==0)){

	  R_observer = (Vd_obs_high_filt-Vd_obs_low_filt)/(2.0f*LR_OBS_CURRENT);
	  L_observer = (Vq_obs_high_filt-Vq_obs_low_filt-6.28f*(_motor->FOC.eHz-Last_eHz)*_motor->FOC.flux_observed)/(2.0f*LR_OBS_CURRENT*6.28f*_motor->FOC.eHz);

	  	if(plusminus==1){
	  		plusminus = -1;
	  	  Vd_obs_low_filt = Vd_obs_low/LR_collect_count;
	  	  Vq_obs_low_filt = Vq_obs_low/LR_collect_count;
	  	  _motor->FOC.Idq_req.d = _motor->FOC.Idq_req.d+1.0f*LR_OBS_CURRENT;
  		  Vd_obs_low = 0;
  		  Vq_obs_low = 0;
	  	}else if(plusminus == -1){
	  		plusminus = 1;
	  	  Vd_obs_high_filt = Vd_obs_high/LR_collect_count;
	  	  Vq_obs_high_filt = Vq_obs_high/LR_collect_count;
	  	  _motor->FOC.Idq_req.d = _motor->FOC.Idq_req.d-1.0f*LR_OBS_CURRENT;
  		  Vd_obs_high = 0;
  		  Vq_obs_high = 0;
	  	}
	  	Last_eHz = _motor->FOC.eHz;
		  LR_collect_count = 0; //Reset this after doing the calcs
	  }
#if 0
	  	float Rerror = R_observer-_motor->m.R;
	  	float Lerror = L_observer-_motor->m.L_D;
	  	//Apply the correction excluding large changes
	  	if(fabs(Rerror)<0.1f*_motor->m.R){
	  		_motor->m.R = _motor->m.R+0.1f*Rerror;
	  	}else if(fabs(Rerror)<0.5f*_motor->m.R){
	  		_motor->m.R = _motor->m.R+0.001f*Rerror;
	  	}
	  	if(fabs(Lerror)<0.1f*_motor->m.L_D){
	  		_motor->m.L_D = _motor->m.L_D+0.1f*Lerror;
	  		_motor->m.L_Q = _motor->m.L_Q +0.1f*Lerror;
	  	}else if(fabs(Lerror)<0.5f*_motor->m.L_D){
	  		_motor->m.L_D = _motor->m.L_D+0.001f*Lerror;
	  		_motor->m.L_Q = _motor->m.L_Q +0.001f*Lerror;
	  	}

#endif
  }

  void LRObserverCollect(MESC_motor_typedef *_motor){
	  LR_collect_count++;
	  if((fabsf(_motor->FOC.eHz)>0.005f*_motor->FOC.pwm_frequency)&&(_motor->FOC.inject ==0)){
	  	  	if(plusminus==1){
	  	  		Vd_obs_low = Vd_obs_low + _motor->FOC.Vdq.d;
	  	  		Vq_obs_low = Vq_obs_low + _motor->FOC.Vdq.q;
	  	  	}
	  	  	if(plusminus == -1){
	  	  		Vd_obs_high = Vd_obs_high + _motor->FOC.Vdq.d;
	  	  		Vq_obs_high = Vq_obs_high + _motor->FOC.Vdq.q;
	  	  	}
	  }
  }

  void HallFluxMonitor(MESC_motor_typedef *_motor){
	  if(fabsf(_motor->FOC.Vdq.q)>10.0f){ //Are we actually spinning at a reasonable pace?
		  if((_motor->hall.current_hall_state>0)&&(_motor->hall.current_hall_state<7)){
	  _motor->m.hall_flux[_motor->hall.current_hall_state - 1][0] =
			  0.999f*_motor->m.hall_flux[_motor->hall.current_hall_state - 1][0] +
			  0.001f*_motor->FOC.flux_a;
	  //take a slow average of the alpha flux linked and store it for later preloading
	  //the observer during very low speed conditions. There is a slight bias towards
	  //later values of flux linked, which is probably good.
	  _motor->m.hall_flux[_motor->hall.current_hall_state - 1][1] =
			  0.999f*_motor->m.hall_flux[_motor->hall.current_hall_state - 1][1] +
			  0.001f*_motor->FOC.flux_b;
		  }
		  _motor->FOC.hall_initialised = 1;
	  }
  }


void  logVars(MESC_motor_typedef *_motor){
	sampled_vars.Vbus[sampled_vars.current_sample] = _motor->Conv.Vbus;
	sampled_vars.Iu[sampled_vars.current_sample] = _motor->Conv.Iu;
	sampled_vars.Iv[sampled_vars.current_sample] = _motor->Conv.Iv;
	sampled_vars.Iw[sampled_vars.current_sample] = _motor->Conv.Iw;
	sampled_vars.Vd[sampled_vars.current_sample] = _motor->FOC.Vdq.d;
	sampled_vars.Vq[sampled_vars.current_sample] = _motor->FOC.Vdq.q;
	sampled_vars.angle[sampled_vars.current_sample] = _motor->FOC.FOCAngle;
	sampled_vars.current_sample++;
	if(sampled_vars.current_sample>=LOGLENGTH){
		sampled_vars.current_sample = 0;
	}
}

int samples_sent;
uint32_t start_ticks;
extern DMA_HandleTypeDef hdma_usart3_tx;
void printSamples(UART_HandleTypeDef *uart, DMA_HandleTypeDef *dma){
#ifdef LOGGING
	char send_buffer[100];
	uint16_t length;
	if(print_samples_now){
		print_samples_now = 0;
		lognow = 0;
		int current_sample_pos = sampled_vars.current_sample;
		start_ticks = HAL_GetTick();

		samples_sent = 0;

		while(samples_sent<LOGLENGTH){
				HAL_Delay(1);//Wait 2ms, would be nice if we could poll for the CDC being free...
				samples_sent++;
				current_sample_pos++;

				if(current_sample_pos>=LOGLENGTH){
					current_sample_pos = 0;	//Wrap
				}

				length = sprintf(send_buffer,"%.2f%.2f,%.2f,%.2f,%.2f,%.2f,%d;\r\n",
						sampled_vars.Vbus[current_sample_pos],
						sampled_vars.Iu[current_sample_pos],
						sampled_vars.Iv[current_sample_pos],
						sampled_vars.Iw[current_sample_pos],
						sampled_vars.Vd[current_sample_pos],
						sampled_vars.Vq[current_sample_pos],
						sampled_vars.angle[current_sample_pos]);
				if((HAL_GetTick()-start_ticks)>10000){
				break;
				}
#ifdef MESC_UART_USB
		CDC_Transmit_FS(send_buffer, length);
#else

		HAL_UART_Transmit_DMA(uart, send_buffer, length);
			while(hdma_usart3_tx.State != HAL_DMA_STATE_READY){//Pause here
		//	while(hdma_usart3_tx.Lock != HAL_UNLOCKED){//Pause here
				__NOP();
			}
#endif
		}
		lognow = 1;
	}
#endif
}

volatile float cnt1=0.0f;
volatile float accu=0.0f;

void RunHFI(MESC_motor_typedef *_motor){
	int Idqreq_dir=0;
	if (_motor->FOC.inject_high_low_now == 0){//First we create the toggle
		_motor->FOC.inject_high_low_now = 1;
		  Idq[0].d = _motor->FOC.Idq.d;
		  Idq[0].q = _motor->FOC.Idq.q;
	}else{
		_motor->FOC.inject_high_low_now = 0;
		  Idq[1].d = _motor->FOC.Idq.d;
		  Idq[1].q = _motor->FOC.Idq.q;
	  }
	dIdq.d = (Idq[0].d - Idq[1].d); //Calculate the changing current levels
	dIdq.q = (Idq[0].q - Idq[1].q);

	switch(_motor->HFIType){
		case HFI_TYPE_NONE:
		__NOP();
		break;
		/////////////////////////////////////////////////////////////////////////////////////////////////////////////
		case HFI_TYPE_45:
			if(_motor->FOC.inject_high_low_now ==1){
				_motor->FOC.Vd_injectionV = +_motor->meas.hfi_voltage;
				if(_motor->FOC.Idq_req.q>0.0f){
					Idqreq_dir = 1;
					_motor->FOC.Vq_injectionV = +_motor->meas.hfi_voltage;
				}else{
					_motor->FOC.Vq_injectionV = -_motor->meas.hfi_voltage;
					Idqreq_dir = -1;
				}
			}else{
				_motor->FOC.Vd_injectionV = -_motor->meas.hfi_voltage;
				if(_motor->FOC.Idq_req.q>0.0f){
					_motor->FOC.Vq_injectionV = -_motor->meas.hfi_voltage;
				}else{
					_motor->FOC.Vq_injectionV = +_motor->meas.hfi_voltage;
				}
			}
			//Run the PLL
			magnitude45 = sqrtf(dIdq.d*dIdq.d+dIdq.q*dIdq.q);

			if(_motor->FOC.was_last_tracking==0){

				float error;
				//Estimate the angle error, the gain to be determined in the HFI detection and setup based on the HFI current and the max iteration allowable
				error = _motor->FOC.HFI_Gain*(magnitude45-_motor->FOC.HFI45_mod_didq);
				if(error>500.0f){error = 500.0f;}
				if(error<-500.0f){error = -500.0f;}
				_motor->FOC.HFI_int_err = _motor->FOC.HFI_int_err +0.05f*error;
				if(_motor->FOC.HFI_int_err>1000.0f){_motor->FOC.HFI_int_err = 1000.0f;}
				if(_motor->FOC.HFI_int_err<-1000.0f){_motor->FOC.HFI_int_err = -1000.0f;}
				_motor->FOC.FOCAngle = _motor->FOC.FOCAngle + (int)(error + _motor->FOC.HFI_int_err)*Idqreq_dir;

			}else{
				_motor->FOC.FOCAngle += _motor->FOC.HFI_test_increment;
				_motor->FOC.HFI_accu += magnitude45;
				_motor->FOC.HFI_count += 1;
			}
			#if 0 //Sometimes for investigation we want to just lock the angle, this is an easy bodge
							_motor->FOC.FOCAngle = 62000;
			#endif
				break;
		/////////////////////////////////////////////////////////////////////////////////////////////////////////////
		case HFI_TYPE_D:
			if(_motor->FOC.inject_high_low_now ==1){
			  _motor->FOC.Vd_injectionV = +_motor->meas.hfi_voltage;
			}else{
			  _motor->FOC.Vd_injectionV = -_motor->meas.hfi_voltage;
			}
			if(dIdq.q>1.0f){dIdq.q = 1.0f;}
			if(dIdq.q<-1.0f){dIdq.q = -1.0f;}
			intdidq.q = (intdidq.q + 0.1f*dIdq.q);
			if(intdidq.q>10){intdidq.q=10;}
			if(intdidq.q<-10){intdidq.q=-10;}
			_motor->FOC.FOCAngle += (int)(250.0f*_motor->FOC.IIR[1] + 10.50f*intdidq.q)*_motor->FOC.d_polarity;
		break;
		case HFI_TYPE_SPECIAL:
			__NOP();
			if(_motor->FOC.inject_high_low_now ==1){
			  _motor->FOC.Vd_injectionV = _motor->FOC.special_injectionVd;
			  _motor->FOC.Vq_injectionV = _motor->FOC.special_injectionVq;
			}else{
				_motor->FOC.Vd_injectionV = -_motor->FOC.special_injectionVd;
				_motor->FOC.Vq_injectionV = -_motor->FOC.special_injectionVq;			}
		break;
	}
}


void SlowHFI(MESC_motor_typedef *_motor){
	/////////////Set and reset the HFI////////////////////////
		switch(_motor->HFIType){
			case HFI_TYPE_45:
			ToggleHFI(_motor);
				if(_motor->FOC.inject==1){
					//static int no_q;
					if(_motor->FOC.was_last_tracking==1){
						if(_motor->FOC.HFI_countdown>0){
							_motor->FOC.HFI45_mod_didq = _motor->FOC.HFI_accu / _motor->FOC.HFI_count;
							_motor->FOC.HFI_Gain = 5000.0f/_motor->FOC.HFI45_mod_didq;
							_motor->FOC.was_last_tracking = 0;
						}else{
							_motor->FOC.HFI_test_increment = 65536 * SLOW_LOOP_FREQUENCY / _motor->FOC.pwm_frequency;
							_motor->FOC.HFI_countdown++;
						}
					}else{
						_motor->FOC.HFI_countdown = 0;
						_motor->FOC.HFI_count = 0;
						_motor->FOC.HFI_accu = 0.0f;
					}
				}
			break;

			case HFI_TYPE_D:
				ToggleHFI(_motor);
				if(_motor->FOC.inject==1){
					if(_motor->FOC.HFI_countdown==3){
						_motor->FOC.Idq_req.d = HFI_TEST_CURRENT;
						_motor->FOC.Idq_req.q=0.0f;//Override the inputs to set Q current to zero
					}else if(_motor->FOC.HFI_countdown==2){
						_motor->FOC.Ldq_now_dboost[0] = _motor->FOC.IIR[0]; //Find the effect of d-axis current
						_motor->FOC.Idq_req.d = 1.0f;
						_motor->FOC.Idq_req.q=0.0f;
					}else if(_motor->FOC.HFI_countdown == 1){
						_motor->FOC.Idq_req.d = -HFI_TEST_CURRENT;
						_motor->FOC.Idq_req.q=0.0f;
					}else if(_motor->FOC.HFI_countdown == 0){
						_motor->FOC.Ldq_now[0] = _motor->FOC.IIR[0];//_motor->FOC.Vd_injectionV;
						_motor->FOC.Idq_req.d = 0.0f;
					if(_motor->FOC.Ldq_now[0]>_motor->FOC.Ldq_now_dboost[0]){_motor->FOC.FOCAngle+=32768;}
					_motor->FOC.HFI_countdown = 200;
					}
					_motor->FOC.HFI_countdown--;
				}
			break;

			case HFI_TYPE_NONE:
				_motor->FOC.inject = 0;
				_motor->FOC.Current_bandwidth = CURRENT_BANDWIDTH;
			break;
			case HFI_TYPE_SPECIAL:
			break;
		}
}

void ToggleHFI(MESC_motor_typedef *_motor){
	if(((_motor->FOC.Vdq.q-_motor->FOC.Idq_smoothed.q*_motor->m.R) > _motor->FOC.HFI_toggle_voltage)||((_motor->FOC.Vdq.q-_motor->FOC.Idq_smoothed.q*_motor->m.R) < -_motor->FOC.HFI_toggle_voltage)||(_motor->MotorSensorMode==MOTOR_SENSOR_MODE_HALL)){
		_motor->FOC.inject = 0;
		_motor->FOC.Current_bandwidth = CURRENT_BANDWIDTH;
	} else if(((_motor->FOC.Vdq.q-_motor->FOC.Idq_smoothed.q*_motor->m.R) < (_motor->FOC.HFI_toggle_voltage-1.0f))&&((_motor->FOC.Vdq.q-_motor->FOC.Idq_smoothed.q*_motor->m.R) > -(_motor->FOC.HFI_toggle_voltage-1.0f)) &&(_motor->HFIType !=HFI_TYPE_NONE)){
		_motor->FOC.inject = 1;
		_motor->FOC.Current_bandwidth = CURRENT_BANDWIDTH*0.1f;
	}
}

static float dinductance, qinductance;

float detectHFI(MESC_motor_typedef *_motor){
	  ///Try out a new detection routine
#if 1

	_motor->meas.previous_HFI_type = _motor->HFIType;
	_motor->HFIType = HFI_TYPE_D;
	input_vars.Idq_req_UART.q = 0.25f;
	int a = 0;
	dinductance = 0;
	qinductance = 0;
	while(a<1000){
		a++;
		_motor->HFIType = HFI_TYPE_D;
		dinductance = dinductance + dIdq.d;
		HAL_Delay(0);
		//input_vars.input_options = 0b
	}
	dinductance = dinductance/1000.0f;
	//dinductance = motor1.FOC.pwm_period*motor1.FOC.Vd_injectionV/(motor1.Conv.Vbus*dinductance);
	//Vdt/di = L
	_motor->FOC.d_polarity = -1;
	a=0;
	while(a<1000){
		a++;
		_motor->HFIType = HFI_TYPE_D;
		qinductance = qinductance + dIdq.d;
		HAL_Delay(0);
		//input_vars.input_options = 0b
	}
	qinductance = qinductance/1000.0f; //Note that this is not yet an inductance, but an inverse of inductance*voltage
	_motor->FOC.HFI45_mod_didq = sqrtf(qinductance*qinductance+dinductance*dinductance);
	_motor->FOC.HFI_Gain = 5000.0f/_motor->FOC.HFI45_mod_didq; //Magic numbers that seem to work
	input_vars.Idq_req_UART.q = 0.0f;
	_motor->FOC.d_polarity = 1;

	_motor->HFIType = _motor->meas.previous_HFI_type;

	return _motor->FOC.HFI45_mod_didq;

#endif
}

void collectInputs(MESC_motor_typedef *_motor){
	  //Collect the requested throttle inputs
	  //UART input
	  if(0 == (input_vars.input_options & 0b1000)){
		  input_vars.Idq_req_UART.q = 0.0f;
	  }

	  //RCPWM input
	  if(input_vars.input_options & 0b0100){
		  if(input_vars.pulse_recieved){
			  if((input_vars.IC_duration > input_vars.IC_duration_MIN) && (input_vars.IC_duration < input_vars.IC_duration_MAX)){
				  if(input_vars.IC_pulse>(input_vars.IC_pulse_MID + input_vars.IC_pulse_DEADZONE)){
					  input_vars.Idq_req_RCPWM.d = 0.0f;
					  input_vars.Idq_req_RCPWM.q = (float)(input_vars.IC_pulse - (input_vars.IC_pulse_MID + input_vars.IC_pulse_DEADZONE))*input_vars.RCPWM_gain[0][1];
				  }
				  else if(input_vars.IC_pulse<(input_vars.IC_pulse_MID - input_vars.IC_pulse_DEADZONE)){
					  input_vars.Idq_req_RCPWM.d = 0.0f;
					  input_vars.Idq_req_RCPWM.q = ((float)input_vars.IC_pulse - (float)(input_vars.IC_pulse_MID - input_vars.IC_pulse_DEADZONE))*input_vars.RCPWM_gain[0][1];
				  }
				  else{
					  input_vars.Idq_req_RCPWM.d = 0.0f;
					  input_vars.Idq_req_RCPWM.q = 0.0f;
				  }
			  }	else {//The duration of the IC was wrong; trap it and write no current request
				  //Todo maybe want to implement a timeout on this, allowing spurious pulses to not wiggle the current?
				  input_vars.Idq_req_RCPWM.d = 0.0f;
				  input_vars.Idq_req_RCPWM.q = 0.0f;
			  }
		  }
		  else {//No pulse received flag
			  input_vars.Idq_req_RCPWM.d = 0.0f;
			  input_vars.Idq_req_RCPWM.q = 0.0f;
		  }
	  }

	  //ADC1 input
	  if(input_vars.input_options & 0b0010){
		  if(_motor->Raw.ADC_in_ext1>input_vars.adc1_MIN){
			  input_vars.Idq_req_ADC1.d = 0.0f;
			  input_vars.Idq_req_ADC1.q = ((float)_motor->Raw.ADC_in_ext1-(float)input_vars.adc1_MIN)*input_vars.adc1_gain[1]*input_vars.ADC1_polarity;
		  }
		  else{
			  input_vars.Idq_req_ADC1.d = 0.0f;
			  input_vars.Idq_req_ADC1.q = 0.0f;
		  }
	  }
	  if(input_vars.input_options & 0b0001){
		  //ADC2 input
		  //placeholder
	  }
}

void RunMTPA(MESC_motor_typedef *_motor){
	//Run MTPA (Field weakening seems to have to go in  the fast loop to be stable)
	#ifdef USE_MTPA

	    if(_motor->m.L_QD>0){
	    	_motor->FOC.id_mtpa = _motor->m.flux_linkage/(4.0f*_motor->m.L_QD) - sqrtf((_motor->m.flux_linkage*_motor->m.flux_linkage/(16.0f*_motor->m.L_QD*_motor->m.L_QD))+_motor->FOC.Idq_req.q*_motor->FOC.Idq_req.q*0.5f);
	    	if(fabsf(_motor->FOC.Idq_req.q)>fabsf(_motor->FOC.id_mtpa)){
	    	_motor->FOC.iq_mtpa = sqrtf(_motor->FOC.Idq_req.q*_motor->FOC.Idq_req.q-_motor->FOC.id_mtpa*_motor->FOC.id_mtpa);
	    	}
	    	else{
	    		_motor->FOC.iq_mtpa = 0;
	    	}
	    _motor->FOC.Idq_req.d = _motor->FOC.id_mtpa;
	    if(_motor->FOC.Idq_req.q>0.0f){
	    _motor->FOC.Idq_req.q = _motor->FOC.iq_mtpa;}
	    else{
	    	_motor->FOC.Idq_req.q = -_motor->FOC.iq_mtpa;}
	    }


	#endif
}
void calculatePower(MESC_motor_typedef *_motor){
////// Calculate the current power
		_motor->FOC.currentPower.d = 1.5f*(_motor->FOC.Vdq.d*_motor->FOC.Idq_smoothed.d);
		_motor->FOC.currentPower.q = 1.5f*(_motor->FOC.Vdq.q*_motor->FOC.Idq_smoothed.q);
		_motor->FOC.Ibus = (_motor->FOC.currentPower.d + _motor->FOC.currentPower.q) /_motor->Conv.Vbus;
}

void LimitFWCurrent(MESC_motor_typedef *_motor){
    //Account for Field weakening current
    //MTPA is already conservative of the current limits
    float mag = (Square(_motor->FOC.Idq_req.q) + Square(_motor->FOC.FW_current));
    if(mag>Square(input_vars.max_request_Idq.q)){
    	float Iqmax2 = Square(input_vars.max_request_Idq.q)-Square(_motor->FOC.FW_current);
    	if(Iqmax2>0){//Avoid hardfault
			if(_motor->FOC.Idq_req.q>0){
				_motor->FOC.Idq_req.q = sqrtf(Iqmax2);
			}else{
				_motor->FOC.Idq_req.q = -sqrtf(Iqmax2);
			}
    	}else{//Negative result, FW larger than allowable current
    		_motor->MotorState = MOTOR_STATE_ERROR;
    		handleError(_motor, ERROR_MATH);
    		_motor->FOC.FW_current = 0.0f;
    	}
    }

}

void clampBatteryPower(MESC_motor_typedef *_motor){
/////// Clamp the max power taken from the battery
    _motor->FOC.reqPower = 1.5f*fabsf(_motor->FOC.Vdq.q * _motor->FOC.Idq_req.q);
    if (_motor->FOC.reqPower > _motor->m.Pmax) {
    	if(_motor->FOC.Idq_req.q > 0.0f){
    		_motor->FOC.Idq_req.q = _motor->m.Pmax / (fabsf(_motor->FOC.Vdq.q)*1.5f);
    	}else{
    		_motor->FOC.Idq_req.q = -_motor->m.Pmax / (fabsf(_motor->FOC.Vdq.q)*1.5f);
    	}
    }
}
void houseKeeping(MESC_motor_typedef *_motor){
	////// Unpuc the observer kludge
	// The observer gets into a bit of a state if it gets close to
	// flux linked = 0 for both accumulators, the angle rapidly changes
	// as it oscillates around zero. Solution... just kludge it back out.
	// This only happens at stationary when it is useless anyway.
	if ((_motor->FOC.flux_a * _motor->FOC.flux_a + _motor->FOC.flux_b * _motor->FOC.flux_b) <
		0.25f * _motor->FOC.flux_observed * _motor->FOC.flux_observed) {
		_motor->FOC.flux_a = 0.5f * _motor->FOC.flux_observed;
		_motor->FOC.flux_b = 0.5f * _motor->FOC.flux_observed;
	}

	//Speed tracker
	if(abs(_motor->FOC.angle_error)>10000){
		//The PLL has run away locking on to aliases; 10000 implies 6.5 pwm periods per sin wave, which is ~3000eHz, 180kerpm at 20kHz PWM frequency.
		//While it IS possible to run faster than this, it is not a sensible use case and will not be supported.
		_motor->FOC.angle_error = 0;
	}
	//Translate the eHz to eRPM
	if(_motor->m.pole_pairs>0){//avoid divide by zero
	_motor->FOC.mechRPM = _motor->FOC.eHz*60.0f/(float)(_motor->m.pole_pairs);
	}
	//Shut down if we are burning the hall sensors //Legacy code, can probably be removed...
	if(getHallState()==0){//This happens when the hall sensors overheat it seems.
	  	  if (MotorError == MOTOR_ERROR_NONE) {
	  		    speed_motor_limiter();
	  	  }
	  	  MotorError = MOTOR_ERROR_HALL0;
	    }else /*if(getHallState()==7){
	  	  MotorError = MOTOR_ERROR_HALL7;
	    } else */{
	  	  if (MotorError != MOTOR_ERROR_NONE) {
	  		  // TODO speed_road();
	  	  }
	  	  MotorError = MOTOR_ERROR_NONE;
	    }
}
void FWRampDown(MESC_motor_typedef *_motor){
	//Ramp down the field weakening current
	//Do NOT assign motorState here, since it could override error states
	_motor->FOC.FW_current*=0.95f;
	if(_motor->FOC.Vdq.q <0.0f){
		_motor->FOC.Idq_req.q = 0.2f; //Apply a brake current
	}
	if(_motor->FOC.Vdq.q >0.0f){
		_motor->FOC.Idq_req.q = -0.2f; //Apply a brake current
	}
}

void ThrottleTemperature(MESC_motor_typedef *_motor){
	//ToDo, do this properly
    if(temp_check( _motor->Raw.MOSu_T ) == false){
    	if(0){generateBreak(_motor); //ToDo Currently not loading the profile so commented out - no temp safety!
    	_motor->MotorState = MOTOR_STATE_ERROR;
    	MotorError = MOTOR_ERROR_OVER_LIMIT_TEMP;
    	}
    }
}


void BLDCCommute(MESC_motor_typedef *_motor){

//Collect the variables required
	switch (_motor->BLDC.sector){
		case 0:
		_motor->BLDC.I_meas = _motor->Conv.Iu;
//		_motor->BLDC.V_meas = _motor->Conv.Vv;
//		_motor->BLDC.rising_int = _motor->BLDC.rising_int + _motor->BLDC.V_meas*_motor->BLDC.PWM_period;
		break;

		case 1:
			_motor->BLDC.I_meas = _motor->Conv.Iu;
//			_motor->BLDC.V_meas = _motor->Conv.Vw;
//			_motor->BLDC.falling_int = _motor->BLDC.rising_int + _motor->BLDC.V_meas*_motor->BLDC.PWM_period;
		break;

		case 2:
			_motor->BLDC.I_meas = _motor->Conv.Iw;
//			_motor->BLDC.V_meas = _motor->Conv.Vu;
//			_motor->BLDC.rising_int = _motor->BLDC.rising_int + _motor->BLDC.V_meas*_motor->BLDC.PWM_period;
		break;

		case 3:
			_motor->BLDC.I_meas = _motor->Conv.Iw;
//			_motor->BLDC.V_meas = _motor->Conv.Vv;
//			_motor->BLDC.falling_int = _motor->BLDC.rising_int + _motor->BLDC.V_meas*_motor->BLDC.PWM_period;
		break;

		case 4:
			_motor->BLDC.I_meas = _motor->Conv.Iv;
//			_motor->BLDC.V_meas = _motor->Conv.Vw;
//			_motor->BLDC.rising_int = _motor->BLDC.rising_int + _motor->BLDC.V_meas*_motor->BLDC.PWM_period;
		break;

		case 5:
			_motor->BLDC.I_meas = _motor->Conv.Iv;
//			_motor->BLDC.V_meas = _motor->Conv.Vu;
//			_motor->BLDC.falling_int = _motor->BLDC.rising_int + _motor->BLDC.V_meas*_motor->BLDC.PWM_period;
		break;
	}
//	//Reduce the rising and falling integrals
//	_motor->BLDC.rising_int = _motor->BLDC.rising_int * 0.999f;
//	_motor->BLDC.falling_int = _motor->BLDC.rising_int * 0.999f;

	//Invert the current since we are measuring the current leaving the motor but controlling the voltage where current is going in
	_motor->BLDC.I_meas = -_motor->BLDC.I_meas;

//////Run PID
	_motor->BLDC.I_pgain = _motor->FOC.Iq_pgain;//Borrow from FOC for now
	_motor->BLDC.I_igain = _motor->FOC.Iq_igain;//Borrow from FOC for now
	_motor->BLDC.PWM_period = _motor->FOC.pwm_period;//Borrow from FOC for now

	//Calculate the error
	_motor->BLDC.I_error = (_motor->BLDC.I_set-_motor->BLDC.I_meas)*_motor->BLDC.I_pgain;
	_motor->BLDC.int_I_error = //Calculate the integral error
			_motor->BLDC.int_I_error + _motor->BLDC.I_error * _motor->BLDC.I_igain * _motor->BLDC.PWM_period;

	_motor->BLDC.V_bldc = _motor->BLDC.int_I_error + _motor->BLDC.I_error;
	//Bounding
	if(_motor->BLDC.V_bldc > 0.95f * _motor->Conv.Vbus){
		_motor->BLDC.V_bldc = 0.95f * _motor->Conv.Vbus;
		if(_motor->BLDC.int_I_error > _motor->Conv.Vbus){
			_motor->BLDC.int_I_error = _motor->Conv.Vbus;
			_motor->BLDC.I_error = 0.05f*_motor->BLDC.int_I_error;
		}
	}
	//Determine the conversion from volts to PWM
	_motor->BLDC.V_bldc_to_PWM = _motor->mtimer->Instance->ARR/_motor->Conv.Vbus;
	//Convert to PWM value
	_motor->BLDC.BLDC_PWM = _motor->BLDC.V_bldc*_motor->BLDC.V_bldc_to_PWM;


//////Integrate and determine if commutation ready, VBEMF = Vbldc-2*I*Rphase
	_motor->BLDC.flux_integral = _motor->BLDC.flux_integral + (_motor->BLDC.V_bldc - _motor->BLDC.I_meas * 2.0f*_motor->m.R)* _motor->BLDC.PWM_period; //Volt seconds

	//FUDGED
	_motor->BLDC.closed_loop = 1;
	if(_motor->BLDC.closed_loop){
		//If the flux reaches a limit  then commute
		if(_motor->BLDC.flux_integral<0.0f){_motor->BLDC.flux_integral = 0.0f;}
		if(_motor->BLDC.flux_integral>_motor->BLDC.com_flux){
			_motor->BLDC.V_meas_sect[_motor->BLDC.sector] = _motor->BLDC.V_meas;

			_motor->BLDC.sector = _motor->BLDC.sector + _motor->BLDC.direction;
			_motor->BLDC.last_flux_integral = _motor->BLDC.flux_integral;
			_motor->BLDC.flux_integral = 0.0f; //Reset the integrator
			_motor->BLDC.last_p_error = _motor->BLDC.I_error;
			//Run a vague tuning mechanism, needs a lot of work.
			if((_motor->BLDC.int_I_error>_motor->Conv.Vbus*0.4f) && (_motor->BLDC.int_I_error<_motor->Conv.Vbus*0.9f)){
				if(_motor->BLDC.I_error>0.05f*_motor->BLDC.int_I_error){
					_motor->BLDC.com_flux = _motor->BLDC.com_flux*1.005f;
				}
				if(_motor->BLDC.I_error<0.05f*_motor->BLDC.int_I_error){
					_motor->BLDC.com_flux = _motor->BLDC.com_flux*0.99f;
				}
			}

//			_motor->BLDC.rising_int_st =_motor->BLDC.rising_int;
//			_motor->BLDC.rising_int = 0.0f;
//			_motor->BLDC.falling_int_st = _motor->BLDC.falling_int;
//			_motor->BLDC.falling_int = 0.0f;
//			if(_motor->BLDC.falling_int_st > _motor->BLDC.falling_int_st){
//				_motor->BLDC.com_flux = _motor->BLDC.com_flux * 1.01f;
//			}else{
//				_motor->BLDC.com_flux = _motor->BLDC.com_flux * 0.99f;
//			}
//
//			if(_motor->BLDC.com_flux<0.018f){_motor->BLDC.com_flux=0.018f;}
//			if(_motor->BLDC.com_flux>0.022f){_motor->BLDC.com_flux=0.022f;}

		}
	}else{
		_motor->BLDC.OL_countdown--;
		if(_motor->BLDC.OL_countdown == 0){
			_motor->BLDC.OL_countdown =_motor->BLDC.OL_periods;
			_motor->BLDC.sector++;
			_motor->BLDC.last_flux_integral = _motor->BLDC.flux_integral;
			_motor->BLDC.flux_integral = 0.0f; //Reset the integrator
		}
	}

//////Wrap the sector
	if(_motor->BLDC.sector>5){
		_motor->BLDC.sector = 0;
	}else if(_motor->BLDC.sector<0){
		_motor->BLDC.sector = 5;
	}

//////Write PWMs
	switch (_motor->BLDC.sector){
		case 0:
			phV_Break(_motor);
			phU_Enable(_motor);
			phW_Enable(_motor);
			_motor->mtimer->Instance->CCR1 = 0;
			_motor->mtimer->Instance->CCR3 = _motor->BLDC.BLDC_PWM;
		break;
		case 1:
			phW_Break(_motor);
			phU_Enable(_motor);
			phV_Enable(_motor);
			_motor->mtimer->Instance->CCR1 = 0;
			_motor->mtimer->Instance->CCR2 = _motor->BLDC.BLDC_PWM;
		break;
		case 2:
			phU_Break(_motor);
			phV_Enable(_motor);
			phW_Enable(_motor);
			_motor->mtimer->Instance->CCR3 = 0;
			_motor->mtimer->Instance->CCR2 = _motor->BLDC.BLDC_PWM;
		break;
		case 3:
			phV_Break(_motor);
			phU_Enable(_motor);
			phW_Enable(_motor);
			_motor->mtimer->Instance->CCR3 = 0;
			_motor->mtimer->Instance->CCR1 = _motor->BLDC.BLDC_PWM;
		break;
		case 4:
			phW_Break(_motor);
			phU_Enable(_motor);
			phV_Enable(_motor);
			_motor->mtimer->Instance->CCR2 = 0;
			_motor->mtimer->Instance->CCR1 = _motor->BLDC.BLDC_PWM;
		break;
		case 5:
			phU_Break(_motor);
			phV_Enable(_motor);
			phW_Enable(_motor);
			_motor->mtimer->Instance->CCR2 = 0;
			_motor->mtimer->Instance->CCR3 = _motor->BLDC.BLDC_PWM;
		break;
		default:
			//Reset to 0, something went wrong...
			_motor->BLDC.sector = 0;
		break;

	}
}

void CalculateBLDCGains(MESC_motor_typedef *_motor){

}



  // clang-format on
