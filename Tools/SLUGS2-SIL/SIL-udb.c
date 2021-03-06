//
//  SIL-udb.c
//  MatrixPilot-SIL
//
//  Created by Ben Levitt on 2/1/13.
//  Copyright (c) 2013 MatrixPilot. All rights reserved.
//

#if (WIN == 1 || NIX == 1)

#include "apUtils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//#include "../../libUDB/libUDB.h"
#include "libUDB.h"
#include "libdcm.h"
//#include "../../libUDB/magnetometer.h"
//#include "../../libUDB/heartbeat.h"
#include "heartbeat.h"

#include "SIL-config.h"
#include "mavlink.h"

/* Include model header file for global data */
#include "AUAV_V3_TestSensors.h"

//mavlink_gps_raw_int_t mlGpsData;       /* '<Root>/mlGpsData' */
extern uint8_t UartOutBuff[MAVLINK_MAX_PACKET_LEN];

#ifdef WIN

#define SIL_WINDOWS_INCS
#include <Windows.h>
#include <Time.h>

////////////////////////////////////////////////////////////////////////////////
// Use variable data width in HILSIM for output channels
//  This is used to support NUM_OUTPUTS > 8
// NOTE: Must have correct version of HILSIM to support this
#define USE_VARIABLE_HILSIM_CHANNELS    0
union dcm_fbts_word dcm_flags;

// gyro rotation vector:
fractional omegagyro[] = { 0, 0, 0 };
static fractional omega[] = { 0, 0, 0 };

//#if (HILSIM == 1)
//#if (USE_VARIABLE_HILSIM_CHANNELS != 1)
//uint8_t SIMservoOutputs[] = {
//	0xFF, 0xEE, //sync
//	0x03, 0x04, //S1
//	0x05, 0x06, //S2
//	0x07, 0x08, //S3
//	0x09, 0x0A, //S4
//	0x0B, 0x0C, //S5
//	0x0D, 0x0E, //S6
//	0x0F, 0x10, //S7
//	0x11, 0x12, //S8
//	0x13, 0x14  //checksum
//};
//#define HILSIM_NUM_SERVOS 8
//#else
//#define HILSIM_NUM_SERVOS NUM_OUTPUTS
//uint8_t SIMservoOutputs[(NUM_OUTPUTS * 2) + 5] = {
//	0xFE, 0xEF, // sync
//	0x00        // output count
//				// Two checksum on the end
//};
//#endif // USE_VARIABLE_HILSIM_CHANNELS
//
//void send_HILSIM_outputs(void);
//#endif // HILSIM

struct timezone
{
	int tz_minuteswest; // of Greenwich
	int tz_dsttime;     // type of dst correction to apply
};

#if 0
int gettimeofday(struct timeval *tp, struct timezone *tzp);
#else
/*
#ifndef _TIMEVAL_DEFINED // also in winsock[2].h
#define _TIMEVAL_DEFINED
struct timeval {
	long tv_sec;
	long tv_usec;
};

#define timerisset(tvp)  ((tvp)->tv_sec || (tvp)->tv_usec)
#define timercmp(tvp, uvp, cmp) \
	(((tvp)->tv_sec != (uvp)->tv_sec) ? \
	 ((tvp)->tv_sec cmp (uvp)->tv_sec) : \
	 ((tvp)->tv_usec cmp (uvp)->tv_usec))
#define timerclear(tvp)  (tvp)->tv_sec = (tvp)->tv_usec = 0
#endif // _TIMEVAL_DEFINED
 */
//void  GetSystemTimeAsFileTime(FILETIME*);

inline int gettimeofday(struct timeval* p, void* tz /* IGNORED */)
{
	union {
		long long ns100; /*time since 1 Jan 1601 in 100ns units */
		FILETIME ft;
	} now;

	GetSystemTimeAsFileTime(&(now.ft));
	p->tv_usec=(long)((now.ns100 / 10LL) % 1000000LL);
	p->tv_sec= (long)((now.ns100-(116444736000000000LL))/10000000LL);
	return 0;
}

#endif // 1/0

#else

#include <sys/time.h>
#include <unistd.h>

#endif // WIN

//#include "../../libUDB/ADchannel.h"
//#include "magnetometerOptions.h"
//#include "../../libUDB/events.h"
#include "SIL-udb.h"
#include "SIL-ui.h"
#include "SIL-events.h"
#include "SIL-eeprom.h"
/* Include model header file for global data */
#include "AUAV_V3_TestSensors.h"

uint16_t udb_heartbeat_counter;

int16_t udb_pwIn[MAX_INPUTS];   // pulse widths of radio inputs
int16_t udb_pwTrim[MAX_INPUTS]; // initial pulse widths for trimming
int16_t udb_pwOut[MAX_OUTPUTS]; // pulse widths for servo outputs

union udb_fbts_byte udb_flags;

struct ADchannel udb_xaccel, udb_yaccel, udb_zaccel;    // x, y, and z accelerometer channels
struct ADchannel udb_xrate, udb_yrate, udb_zrate;       // x, y, and z gyro channels
struct ADchannel udb_vref;                              // reference voltage
struct ADchannel udb_analogInputs[4];

int16_t udb_magFieldBody[3];                    // magnetic field in the body frame of reference
int16_t udb_magOffset[3] = { 0 , 0 , 0 };       // magnetic offset in the body frame of reference
int16_t magGain[3] = { RMAX , RMAX , RMAX };    // magnetometer calibration gains
int16_t rawMagCalib[3] = { 0 , 0 , 0 };
uint8_t magreg[6];                              // magnetometer read-write buffer
int16_t magFieldRaw[3];

union longww battery_current;                   // battery_current._.W1 is in tenths of Amps
union longww battery_mAh_used;                  // battery_mAh_used._.W1 is in mAh
union longww battery_voltage;                   // battery_voltage._.W1 is in tenths of Volts
uint8_t rc_signal_strength;                     // rc_signal_strength is 0-100 as percent of full signal

int16_t magMessage;
int16_t vref_adj;

volatile uint16_t trap_flags;
volatile uint32_t trap_source;
volatile uint16_t osc_fail_count;
uint16_t mp_rcon = 3;                           // default RCON state at normal powerup

extern int mp_argc;
extern char **mp_argv;

uint8_t leds[5] = {0, 0, 0, 0, 0};
UDBSocket serialSocket;
uint8_t sil_radio_on;

boolean handleUDBSockets(void);
uint16_t get_current_milliseconds(void);
void sleep_milliseconds(uint16_t ms);


#define UDB_HW_RESET_ARG "-r=EXTR"

// Functions only included with nv memory.
#if (USE_NV_MEMORY == 1)
UDB_SKIP_FLAGS udb_skip_flags = {0, 0, 0};

void udb_skip_radio_trim(boolean b)
{
	udb_skip_flags.skip_radio_trim = 1;
}

void udb_skip_imu_calibration(boolean b)
{
	udb_skip_flags.skip_imu_cal = 1;
}
#endif

void udb_init(void)
{
	int16_t i;

	// If we were reset:
	if (mp_argc >= 2 && strcmp(mp_argv[1], UDB_HW_RESET_ARG) == 0)
	{
		mp_rcon = 128; // enable just the external/MCLR reset bit
	}

	for (i = 0; i < 4; i++)
	{
		leds[i] = LED_OFF;
	}

	udb_heartbeat_counter = 0;
	udb_flags.B = 0;
	sil_radio_on = 1;

	sil_ui_init(mp_rcon);

	gpsSocket = UDBSocket_init((SILSIM_GPS_RUN_AS_SERVER) ?
	                            UDBSocketUDPServer :
	                            UDBSocketUDPClient,
	                            SILSIM_GPS_PORT,
	                            SILSIM_GPS_HOST,
	                            NULL,
	                            0);
	telemetrySocket = UDBSocket_init((SILSIM_TELEMETRY_RUN_AS_SERVER) ?
	                                  UDBSocketUDPServer :
	                                  UDBSocketUDPClient,
	                                  SILSIM_TELEMETRY_PORT,
	                                  SILSIM_TELEMETRY_HOST,
	                                  NULL,
	                                  0);
	if (strlen(SILSIM_SERIAL_RC_INPUT_DEVICE) > 0)
	{
		serialSocket = UDBSocket_init(UDBSocketSerial,
		                              0,
		                              NULL,
		                              SILSIM_SERIAL_RC_INPUT_DEVICE,
		                              SILSIM_SERIAL_RC_INPUT_BAUD);
	}

	/* Initialize model */
	AUAV_V3_TestSensors_initialize();
}

#define UDB_WRAP_TIME 1000
//#define UDB_STEP_TIME 25
#define UDB_STEP_TIME (UDB_WRAP_TIME/HEARTBEAT_HZ)

int initialised = 0;

void udb_run(void)
{
	uint16_t currentTime;
	static uint16_t nextHeartbeatTime;
	uint16_t wrote = 0;
	

	if (!initialised)
	{
		initialised = 1;
		if (strlen(SILSIM_SERIAL_RC_INPUT_DEVICE) == 0)
		{
			udb_pwIn[THROTTLE_INPUT_CHANNEL] = 2000;
			udb_pwTrim[THROTTLE_INPUT_CHANNEL] = 2000;
		}
		nextHeartbeatTime = get_current_milliseconds();
	}

//	while (1) {
		if (!handleUDBSockets())
		{
			sleep_milliseconds(1);
		}

		currentTime = get_current_milliseconds();

		AUAV_V3_TestSensors_DWork.time_since_boot_usec = get_current_microseconds();


		if (currentTime >= nextHeartbeatTime &&
		    !(nextHeartbeatTime <= UDB_STEP_TIME && 
		    currentTime >= UDB_WRAP_TIME-UDB_STEP_TIME))
		{
			

			
			//udb_callback_read_sensors();

			udb_flags._.radio_on = (sil_radio_on && 
			    udb_pwIn[FAILSAFE_INPUT_CHANNEL] >= FAILSAFE_INPUT_MIN && 
			    udb_pwIn[FAILSAFE_INPUT_CHANNEL] <= FAILSAFE_INPUT_MAX);

			LED_GREEN = (udb_flags._.radio_on) ? LED_ON : LED_OFF;

			//udb_heartbeat_40hz_callback(); // Run at 40Hz
			//udb_heartbeat_callback(); // Run at HEARTBEAT_HZ
			_T2Interrupt();
#if (HILSIM == 1)
			//send_HILSIM_outputs();
#endif
			sil_ui_update();
			mlPilotConsoleData.chan3_raw = (udb_pwIn[THROTTLE_INPUT_CHANNEL]);// -1886.0) / 1484.0;
			mlPilotConsoleData.chan1_raw = (udb_pwIn[AILERON_INPUT_CHANNEL]);// -2628.6666666666674) / -2792.21432160421;
			mlPilotConsoleData.chan4_raw = (udb_pwIn[RUDDER_INPUT_CHANNEL]);// -2629.666666666667) / -2102.7551081301217;
			mlPilotConsoleData.chan2_raw = (udb_pwIn[ELEVATOR_INPUT_CHANNEL]);// -2628.0000000000005) / 4159.6735926497768;
			mlPilotConsoleData.chan5_raw = udb_pwIn[MODE_SWITCH_INPUT_CHANNEL];

//			if (udb_heartbeat_counter % 80 == 0)
			if (udb_heartbeat_counter % (2 * HEARTBEAT_HZ) == 0)
			{
				//writeEEPROMFileIfNeeded(); // Run at 0.5Hz
			}
			//SLUGS2
			if (udb_heartbeat_counter % (HEARTBEAT_HZ / 40) == 0)
			{
				//wrote = PackHeartBeat(/*system_id*/101, /*component_id*/1); // Run at 1Hz
				//mavlink_serial_send(MAVLINK_COMM_0, &UartOutBuff[0], (uint16_t)wrote);

				/* Outputs for Atomic SubSystem: '<Root>/Mavlink_TX_Adapter' */
				//AUAV_V3_Mavlink_TX_AdapterTID9();
				
				//AUAV_V3_Mavlink_TX_AdapterTID5();

				/* End of Outputs for SubSystem: '<Root>/Mavlink_TX_Adapter' */
			}

			if (udb_heartbeat_counter % (HEARTBEAT_HZ / 10) == 0)
			{

				//wrote = PackGpsRawInt(/*system_id*/101, /*component_id*/1, mlGpsData, currentTime); // Run at 1Hz
				//mavlink_serial_send(MAVLINK_COMM_0, &UartOutBuff[0], (uint16_t)wrote);

				//AUAV_V3_Mavlink_TX_AdapterTID4();
			}

			if (udb_heartbeat_counter % (HEARTBEAT_HZ / 5) == 0)
			{
								
				//AUAV_V3_Mavlink_TX_AdapterTID10();
			}
			

			udb_heartbeat_counter++;
			nextHeartbeatTime = nextHeartbeatTime + UDB_STEP_TIME;
			if (nextHeartbeatTime > UDB_WRAP_TIME) 
				nextHeartbeatTime -= UDB_WRAP_TIME;
		}
		

		//SLUGS2
		//if (wrote>0)
			//Sync_SendSerial();

		process_queued_events();
//	}
}


//#if (HILSIM == 1)
//
//void send_HILSIM_outputs(void)
//{
//	// Setup outputs for HILSIM
//	int16_t i;
//	uint8_t CK_A = 0;
//	uint8_t CK_B = 0;
//	union intbb TempBB;
//
//#if (USE_VARIABLE_HILSIM_CHANNELS != 1)
//	udb_pwOut[AILERON_OUTPUT_CHANNEL] = (mlPwmCommands.servo2_raw) * 10;//  + mlPwmCommands.servo6_raw / 2) * 5 ;//
//	udb_pwOut[THROTTLE_OUTPUT_CHANNEL] = (mlPwmCommands.servo1_raw) * 10;// + mlPwmCommands.servo5_raw / 2) * 5;//
//	udb_pwOut[RUDDER_OUTPUT_CHANNEL] = (mlPwmCommands.servo3_raw) * 10;// + mlPwmCommands.servo7_raw / 2) * 5;//
//	udb_pwOut[ELEVATOR_OUTPUT_CHANNEL] = (mlPwmCommands.servo4_raw) * 10;// + mlPwmCommands.servo8_raw / 2) * 5 ;//
//	for (i = 1; i <= NUM_OUTPUTS; i++)
//	{
//		
//		TempBB.BB = udb_pwOut[i];
//		SIMservoOutputs[2 * i] = TempBB._.B1;
//		SIMservoOutputs[(2 * i) + 1] = TempBB._.B0;
//	}
//
//	for (i = 2; i < HILSIM_NUM_SERVOS * 2 + 2; i++)
//	{
//		CK_A += SIMservoOutputs[i];
//		CK_B += CK_A;
//	}
//	SIMservoOutputs[i] = CK_A;
//	SIMservoOutputs[i + 1] = CK_B;
//
//	// Send HILSIM outputs
//	gpsoutbin(HILSIM_NUM_SERVOS * 2 + 4, SIMservoOutputs);
//#else
//	for (i = 1; i <= NUM_OUTPUTS; i++)
//	{
//		TempBB.BB = udb_pwOut[i];
//		SIMservoOutputs[(2 * i) + 1] = TempBB._.B1;
//		SIMservoOutputs[(2 * i) + 2] = TempBB._.B0;
//	}
//
//	SIMservoOutputs[2] = NUM_OUTPUTS;
//
//	// Calcualte checksum
//	for (i = 3; i < (NUM_OUTPUTS * 2) + 3; i++)
//	{
//		CK_A += SIMservoOutputs[i];
//		CK_B += CK_A;
//	}
//	SIMservoOutputs[i] = CK_A;
//	SIMservoOutputs[i + 1] = CK_B;
//
//	// Send HILSIM outputs
//	gpsoutbin((HILSIM_NUM_SERVOS * 2) + 5, SIMservoOutputs);
//#endif // USE_VARIABLE_HILSIM_CHANNELS
//}
//
//#endif // HILSIM
//
//
//void udb_background_trigger(background_callback callback)
//{
//	if (callback) callback();
//}

uint8_t udb_cpu_load(void)
{
	return 5; // sounds reasonable for a fake cpu%
}

int16_t udb_servo_pulsesat(int32_t pw)
{
	if (pw > SERVOMAX) pw = SERVOMAX;
	if (pw < SERVOMIN) pw = SERVOMIN;
	return (int16_t)pw;
}

void udb_servo_record_trims(void)
{
	int16_t i;

	for (i = 1; i <= NUM_INPUTS; i++)
	{
		udb_pwTrim[i] = udb_pwIn[i];
//		DPRINT("udb_pwTrim[%i] = %u\r\n", i, udb_pwTrim[i]);
	}
}

void udb_set_action_state(boolean newValue)
{
	// not simulated
	(void)newValue;   // unused parameter
}

void udb_a2d_record_offsets(void)
{
	UDB_XACCEL.offset = UDB_XACCEL.value;
	udb_xrate.offset  = udb_xrate.value;
	UDB_YACCEL.offset = UDB_YACCEL.value - (Y_GRAVITY_SIGN ((int16_t)(2*GRAVITY))); // opposite direction
	udb_yrate.offset  = udb_yrate.value;
	UDB_ZACCEL.offset = UDB_ZACCEL.value;
	udb_zrate.offset  = udb_zrate.value;
	udb_vref.offset   = udb_vref.value;
}

uint16_t get_reset_flags(void)
{
	return mp_rcon;
}

void sil_reset(void)
{
	const char *args[3] = {mp_argv[0], UDB_HW_RESET_ARG, 0};

	sil_ui_will_reset();

	if (gpsSocket)       UDBSocket_close(gpsSocket);
	if (telemetrySocket) UDBSocket_close(telemetrySocket);
	if (serialSocket)    UDBSocket_close(serialSocket);

	execv(mp_argv[0], args);
	fprintf(stderr, "Failed to reset UDB %s\n", mp_argv[0]);
	exit(1);
}

// time functions
uint16_t get_current_milliseconds(void)
{
	// *nix / mac implementation
	struct timeval tv;
	struct timezone tz;

	gettimeofday(&tv,&tz);
	return tv.tv_usec / 1000;
}

// time functions
uint64_t get_current_microseconds(void)
{
	// *nix / mac implementation
	struct timeval tv;
	struct timezone tz;

	gettimeofday(&tv, &tz);
	return tv.tv_usec ;
}

void sleep_milliseconds(uint16_t ms)
{
#ifdef WIN
	// windows implementation
	Sleep(ms);

#else
	// *nix / mac implementation
	usleep(1000*ms);
#endif
}

void sil_handle_serial_rc_input(uint8_t *buffer, int bytesRead)
{
	int i;

	uint8_t CK_A = 0;
	uint8_t CK_B = 0;
	uint8_t headerBytes = 0;
	uint8_t numServos = 0;

	if (bytesRead >= 2 && buffer[0] == 0xFF && buffer[1] == 0xEE)
	{
		headerBytes = 2;
		numServos = 8;
	}
	else if (bytesRead >= 3 && buffer[0] == 0xFE && buffer[1] == 0xEF)
	{
		headerBytes = 3;
		numServos = buffer[2];
	}

	if (numServos && bytesRead >= headerBytes + numServos*2 + 2)
	{
		for (i = headerBytes; i < headerBytes + numServos*2; i++)
		{
			CK_A += buffer[i];
			CK_B += CK_A;
		}
		if (CK_A == buffer[headerBytes + numServos*2] &&
		    CK_B == buffer[headerBytes + numServos*2 + 1])
		{
			for (i = 1; i <= numServos; i++)
			{
				udb_pwIn[i] = (uint16_t)(buffer[headerBytes + (i-1)*2])*256 +
				                         buffer[headerBytes + (i-1)*2 + 1];
			}
		}
	}
}

#define BUFLEN 512

boolean handleUDBSockets(void)
{
	uint8_t buffer[BUFLEN];
	int32_t bytesRead;
	int16_t i;
	boolean didRead = false;

	// Handle GPS Socket
	if (gpsSocket) {
		bytesRead = UDBSocket_read(gpsSocket, buffer, BUFLEN);
		if (bytesRead < 0) {
			UDBSocket_close(gpsSocket);
			gpsSocket = NULL;
		} else {
			for (i = 0; i < bytesRead; i++) {
				udb_gps_callback_received_byte(buffer[i]);
			}
			if (bytesRead > 0) didRead = true;
		}
	}
	// Handle Telemetry Socket
	if (telemetrySocket) {
		bytesRead = UDBSocket_read(telemetrySocket, buffer, BUFLEN);
		if (bytesRead < 0) {
			UDBSocket_close(telemetrySocket);
			telemetrySocket = NULL;
		} else {
			for (i = 0; i < bytesRead; i++) {
				udb_serial_callback_received_byte(buffer[i]);
			}
			if (bytesRead>0) didRead = true;
		}
	}
	// Handle optional Serial RC input Socket
	if (serialSocket) {
		bytesRead = UDBSocket_read(serialSocket, buffer, BUFLEN);
		if (bytesRead < 0) {
			UDBSocket_close(serialSocket);
			serialSocket = NULL;
		} else {
			if (bytesRead>0) {
				sil_handle_serial_rc_input(buffer, bytesRead);
				didRead = true;
			}
		}
	}
	return didRead;
}

//void udb_callback_read_sensors(void)
//{
//	float T, p, r, h,v;
//	//read_gyros(); // record the average values for both DCM and for offset measurements
//	//read_accel();
//	HILSIM_set_gplane();
//	HILSIM_set_omegagyro();
//	h = mlGpsData.alt / 1000;
//	T = 15.04 - .00649 * h;
//	p = 101.29 *myPow((T + 273.1) / 288.08,5.256) * 1000; //pascal
//	r = p / (.2869 * (T + 273.1)) /1000;//kg/m^3
//	v = mlGpsData.vel / 100;
//
//	mlRawPressureData.press_abs = (p ) / 27.1270 ; //convert to static pressure in Pascal / baroScale
//	mlRawPressureData.press_diff1 = (1 / 2 * r * myPow(v, 2) +1005.9) / 1.0514 ;//pitotScale
//	mlRawPressureData.temperature = (T + 1605.3) / 1.5113;
//	mlAirData.press_diff = ( r * myPow(v, 2))/2 / 100;//hectopascal (1 hPa = 100 Pa)
//	mlAirData.press_abs = (p ) / 100;//hectopascal (1 hPa = 100 Pa)
//	mlAirData.temperature = T*100;//0.01 degrees celsius
//}

#if (MAG_YAW_DRIFT == 1)

static magnetometer_callback_funcptr magnetometer_callback = NULL;

void rxMagnetometer(magnetometer_callback_funcptr callback)
{
	magnetometer_callback = callback;
}

void I2C_doneReadMagData(void)
{
	magFieldRaw[0] = (magreg[0]<<8) + magreg[1];
	magFieldRaw[1] = (magreg[2]<<8) + magreg[3];
	magFieldRaw[2] = (magreg[4]<<8) + magreg[5];

	if (magMessage == 7)
	{
		udb_magFieldBody[0] = MAG_X_SIGN((__builtin_mulsu((magFieldRaw[MAG_X_AXIS]), magGain[MAG_X_AXIS]))>>14)-(udb_magOffset[0]>>1);
		udb_magFieldBody[1] = MAG_Y_SIGN((__builtin_mulsu((magFieldRaw[MAG_Y_AXIS]), magGain[MAG_Y_AXIS]))>>14)-(udb_magOffset[1]>>1);
		udb_magFieldBody[2] = MAG_Z_SIGN((__builtin_mulsu((magFieldRaw[MAG_Z_AXIS]), magGain[MAG_Z_AXIS]))>>14)-(udb_magOffset[2]>>1);

		if ((abs(udb_magFieldBody[0]) < MAGNETICMAXIMUM) &&
			(abs(udb_magFieldBody[1]) < MAGNETICMAXIMUM) &&
			(abs(udb_magFieldBody[2]) < MAGNETICMAXIMUM))
		{
//			udb_magnetometer_callback();
			if (magnetometer_callback != NULL)
			{
				magnetometer_callback();
			}
			else
			{
				printf("ERROR: magnetometer_callback function pointer not set\r\n");
			}
		}
		else
		{
			magMessage = 0;         // invalid reading, reset the magnetometer
		}
	}
}

void HILSIM_MagData(magnetometer_callback_funcptr callback)
{
	(void)callback;
//	magnetometer_callback = callback;
	magMessage = 7;                 // indicate valid magnetometer data
	I2C_doneReadMagData();          // run the magnetometer computations
}

#endif // MAG_YAW_DRIFT

int setjmp(void)
{
	return 0;
}

int16_t FindFirstBitFromLeft(int16_t val)
{
	int16_t i = 0;

	if (val != 0) 
	{
		for (i = 1; i <= 16; i++)
		{
			if (val & 0x8000) break;
			val <<= 1;
		}
	}
	return i;
}

void init_servoPrepare(void) // initialize the PWM
{
	int16_t i;

#if (USE_NV_MEMORY == 1)
	if (udb_skip_flags.skip_radio_trim == 1)
		return;
#endif

	for (i = 0; i <= NUM_INPUTS; i++)
	{
#if (FIXED_TRIMPOINT == 1)
		udb_pwTrim[i] = udb_pwIn[i] = ((i == THROTTLE_INPUT_CHANNEL) ? THROTTLE_TRIMPOINT : CHANNEL_TRIMPOINT);
#else
		udb_pwIn[i] = udb_pwTrim[i] = ((i == THROTTLE_INPUT_CHANNEL) ? 0 : 3000);
#endif
	}

	for (i = 0; i <= NUM_OUTPUTS; i++)
	{
#if (FIXED_TRIMPOINT == 1)
		udb_pwOut[i] = ((i == THROTTLE_OUTPUT_CHANNEL) ? THROTTLE_TRIMPOINT : CHANNEL_TRIMPOINT);
#else
		udb_pwOut[i] = ((i == THROTTLE_OUTPUT_CHANNEL) ? 0 : 3000);
#endif
	}
	udb_pwIn[MODE_SWITCH_INPUT_CHANNEL] = 1000;

#if (NORADIO == 1)
	udb_pwIn[MODE_SWITCH_INPUT_CHANNEL] = udb_pwTrim[MODE_SWITCH_INPUT_CHANNEL] = 4000;
#endif

	mlPilotConsoleData.chan3_raw = udb_pwIn[THROTTLE_INPUT_CHANNEL];
	mlPilotConsoleData.chan1_raw = udb_pwIn[AILERON_INPUT_CHANNEL];
	mlPilotConsoleData.chan4_raw = udb_pwIn[RUDDER_INPUT_CHANNEL];
	mlPilotConsoleData.chan2_raw = udb_pwIn[ELEVATOR_INPUT_CHANNEL];
	mlPilotConsoleData.chan5_raw = udb_pwIn[MODE_SWITCH_INPUT_CHANNEL];
}

#endif // (WIN == 1 || NIX == 1)
