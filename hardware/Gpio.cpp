/*
This code implements basic I/O hardware via the Raspberry Pi's GPIO port, using the wiringpi library.
WiringPi is Copyright (c) 2012-2013 Gordon Henderson. <projects@drogon.net>
It must be installed beforehand following instructions at http://wiringpi.com/download-and-install/

    Copyright (C) 2014 Vicne <vicnevicne@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
    MA 02110-1301 USA.

This is a derivative work based on the samples included with wiringPi where distributed
under the GNU Lesser General Public License version 3
Source: http://wiringpi.com

Connection information:
	This hardware uses the pins of the Raspebrry Pi's GPIO port.
	Please read:
	http://wiringpi.com/pins/special-pin-functions/
	http://wiringpi.com/pins/ for more information
	As we cannot assume domoticz runs as root (which is bad), we won't take advantage of wiringPi's numbering.
	Consequently, we will always use the internal GPIO pin numbering as noted the board, including in the commands below

	Pins have to be exported and configured beforehand and upon each reboot of the Pi, like so:
	- For output pins, only one command is needed:
	gpio export <pin> out

	- For input pins, 2 commands are needed (one to export as input, and one to trigger interrupts on both edges):
	gpio export <pin> in
	gpio edge <pin> both

	Note: If you wire a pull-up, make sure you use 3.3V from P1-01, NOT the 5V pin ! The inputs are 3.3V max !
*/
#include "stdafx.h"
#ifdef WITH_GPIO
#include "Gpio.h"
#include "GpioPin.h"
#ifndef WIN32
#include <wiringPi.h>
#endif
#include "../main/Helper.h"
#include "../main/Logger.h"
#include "hardwaretypes.h"
#include "../main/RFXtrx.h"
#include "../main/localtime_r.h"
#include "../main/mainworker.h"
#include "../main/SQLHelper.h"

#define NO_INTERRUPT	-1
#define MAX_GPIO	31

bool m_bIsInitGPIOPins=false;
bool interruptHigh[MAX_GPIO+1]={ false };
uint32_t m_debounce;
uint32_t m_period;
uint32_t m_pollinterval;

// List of GPIO pin numbers, ordered as listed
std::vector<CGpioPin> CGpio::pins;

// Queue of GPIO numbers which have triggered at least an interrupt to be processed.
std::vector<int> gpioInterruptQueue;

boost::mutex interruptQueueMutex;
boost::condition_variable interruptCondition;

// struct timers for all GPIO pins
struct timeval tvBegin[MAX_GPIO+1], tvEnd[MAX_GPIO+1], tvDiff[MAX_GPIO+1];

/*
 * Direct GPIO implementation, inspired by other hardware implementations such as PiFace and EnOcean
 */
CGpio::CGpio(const int ID, const int debounce, const int period, const int pollinterval)
{
	m_stoprequested=false;
	m_HwdID=ID;
	m_debounce=debounce;
	m_period=period;
	m_pollinterval = pollinterval;

	//Prepare a generic packet info for LIGHTING1 packet, so we do not have to do it for every packet
	IOPinStatusPacket.LIGHTING1.packetlength = sizeof(IOPinStatusPacket.LIGHTING1) -1;
	IOPinStatusPacket.LIGHTING1.housecode = 0;
	IOPinStatusPacket.LIGHTING1.packettype = pTypeLighting1;
	IOPinStatusPacket.LIGHTING1.subtype = sTypeIMPULS;
	IOPinStatusPacket.LIGHTING1.rssi = 12;
	IOPinStatusPacket.LIGHTING1.seqnbr = 0;

	if (!m_bIsInitGPIOPins)
	{
		InitPins();
		m_bIsInitGPIOPins=true;
	}
}

CGpio::~CGpio(void)
{
}

 /*
  * interrupt timer functions:
	*********************************************************************************
  */
int getclock(struct timeval *tv) {
#ifdef CLOCK_MONOTONIC
	struct timespec ts;
		if (!clock_gettime(CLOCK_MONOTONIC, &ts)) {
			tv->tv_sec = ts.tv_sec;
			tv->tv_usec = ts.tv_nsec / 1000;
			return 0;
		}
#endif
	return gettimeofday(tv, NULL);
}
int timeval_subtract (struct timeval *result, struct timeval *x, struct timeval *y) {
	/* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y->tv_usec) {
		int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    y->tv_usec += 1000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}

/*
 * interrupt handlers:
 *********************************************************************************
 */

 void pushInterrupt(int gpioId) {
	getclock(&tvEnd[gpioId]);
	if (timeval_subtract(&tvDiff[gpioId], &tvEnd[gpioId], &tvBegin[gpioId])) {
		tvDiff[gpioId].tv_sec = 0;
		tvDiff[gpioId].tv_usec = 0;
	}
	unsigned int diff = tvDiff[gpioId].tv_usec + tvDiff[gpioId].tv_sec * 1000000;
	getclock(&tvBegin[gpioId]);
	boost::mutex::scoped_lock lock(interruptQueueMutex);
	if (diff>m_period*1000) {
		interruptHigh[gpioId]=false;
		if(std::find(gpioInterruptQueue.begin(), gpioInterruptQueue.end(), gpioId) != gpioInterruptQueue.end()) {
			_log.Log(LOG_NORM, "GPIO: Interrupt for GPIO %d already queued. Ignoring...", gpioId);
		}
		else {
			// Queue interrupt. Note that as we make sure it contains only unique numbers, it can never "overflow".
			_log.Log(LOG_NORM, "GPIO: Queuing interrupt for GPIO %d.", gpioId);
			gpioInterruptQueue.push_back(gpioId);
		}
	}
	else {
		if (!interruptHigh[gpioId]) {
			// _log.Log(LOG_NORM, "GPIO: Too many interrupts for GPIO %d. Ignoring..", gpioId);
			interruptHigh[gpioId]=true;
		}
		interruptCondition.notify_one();
		return;
	}
	interruptCondition.notify_one();
	// _log.Log(LOG_NORM, "GPIO: %d interrupts in queue.", gpioInterruptQueue.size());
}

void interruptHandler0 (void) { pushInterrupt(0); }
void interruptHandler1 (void) { pushInterrupt(1); }
void interruptHandler2 (void) { pushInterrupt(2); }
void interruptHandler3 (void) { pushInterrupt(3); }
void interruptHandler4 (void) { pushInterrupt(4); }
void interruptHandler5 (void) { pushInterrupt(5); }
void interruptHandler6 (void) { pushInterrupt(6); }
void interruptHandler7 (void) { pushInterrupt(7); }
void interruptHandler8 (void) { pushInterrupt(8); }
void interruptHandler9 (void) { pushInterrupt(9); }
void interruptHandler10(void) { pushInterrupt(10); }
void interruptHandler11(void) { pushInterrupt(11); }
void interruptHandler12(void) { pushInterrupt(12); }
void interruptHandler13(void) { pushInterrupt(13); }
void interruptHandler14(void) { pushInterrupt(14); }
void interruptHandler15(void) { pushInterrupt(15); }
void interruptHandler16(void) { pushInterrupt(16); }
void interruptHandler17(void) { pushInterrupt(17); }
void interruptHandler18(void) { pushInterrupt(18); }
void interruptHandler19(void) { pushInterrupt(19); }
void interruptHandler20(void) { pushInterrupt(20); }
void interruptHandler21(void) { pushInterrupt(21); }
void interruptHandler22(void) { pushInterrupt(22); }
void interruptHandler23(void) { pushInterrupt(23); }
void interruptHandler24(void) { pushInterrupt(24); }
void interruptHandler25(void) { pushInterrupt(25); }
void interruptHandler26(void) { pushInterrupt(26); }
void interruptHandler27(void) { pushInterrupt(27); }
void interruptHandler28(void) { pushInterrupt(28); }
void interruptHandler29(void) { pushInterrupt(29); }
void interruptHandler30(void) { pushInterrupt(30); }
void interruptHandler31(void) { pushInterrupt(31); }


bool CGpio::StartHardware()
{
#ifndef WIN32
	// TODO make sure the WIRINGPI_CODES environment variable is set, otherwise WiringPi makes the program exit upon error
	// Note : We're using the wiringPiSetupSys variant as it does not require root privilege
	if (wiringPiSetupSys() != 0) {
		_log.Log(LOG_ERROR, "GPIO: Error initializing wiringPi!");
		return false;
	}
#endif
	m_stoprequested=false;

	//  Start worker thread that will be responsible for interrupt handling
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&CGpio::Do_Work, this)));

	m_bIsStarted=true;
#ifndef WIN32
	//Hook up interrupt call-backs for each input GPIO
	for(std::vector<CGpioPin>::iterator it = pins.begin(); it != pins.end(); ++it) {
		if (it->GetIsExported() && it->GetIsInput()) {
			//	_log.Log(LOG_NORM, "GPIO: Hooking interrupt handler for GPIO %d.", it->GetId());
			switch (it->GetId()) {
				case 0: wiringPiISR(0, INT_EDGE_SETUP, &interruptHandler0); getclock(&tvBegin[0]); break;
				case 1: wiringPiISR(1, INT_EDGE_SETUP, &interruptHandler1); getclock(&tvBegin[1]); break;
				case 2: wiringPiISR(2, INT_EDGE_SETUP, &interruptHandler2); getclock(&tvBegin[2]); break;
				case 3: wiringPiISR(3, INT_EDGE_SETUP, &interruptHandler3); getclock(&tvBegin[3]); break;
				case 4: wiringPiISR(4, INT_EDGE_SETUP, &interruptHandler4); getclock(&tvBegin[4]); break;
				case 5: wiringPiISR(5, INT_EDGE_SETUP, &interruptHandler5); getclock(&tvBegin[5]); break;
				case 6: wiringPiISR(6, INT_EDGE_SETUP, &interruptHandler6); getclock(&tvBegin[6]); break;
				case 7: wiringPiISR(7, INT_EDGE_SETUP, &interruptHandler7); getclock(&tvBegin[7]); break;
				case 8: wiringPiISR(8, INT_EDGE_SETUP, &interruptHandler8); getclock(&tvBegin[8]); break;
				case 9: wiringPiISR(9, INT_EDGE_SETUP, &interruptHandler9); getclock(&tvBegin[9]); break;
				case 10: wiringPiISR(10, INT_EDGE_SETUP, &interruptHandler10); getclock(&tvBegin[10]); break;
				case 11: wiringPiISR(11, INT_EDGE_SETUP, &interruptHandler11); getclock(&tvBegin[11]); break;
				case 12: wiringPiISR(12, INT_EDGE_SETUP, &interruptHandler12); getclock(&tvBegin[12]); break;
				case 13: wiringPiISR(13, INT_EDGE_SETUP, &interruptHandler13); getclock(&tvBegin[13]); break;
				case 14: wiringPiISR(14, INT_EDGE_SETUP, &interruptHandler14); getclock(&tvBegin[14]); break;
				case 15: wiringPiISR(15, INT_EDGE_SETUP, &interruptHandler15); getclock(&tvBegin[15]); break;
				case 16: wiringPiISR(16, INT_EDGE_SETUP, &interruptHandler16); getclock(&tvBegin[16]); break;
				case 17: wiringPiISR(17, INT_EDGE_SETUP, &interruptHandler17); getclock(&tvBegin[17]); break;
				case 18: wiringPiISR(18, INT_EDGE_SETUP, &interruptHandler18); getclock(&tvBegin[18]); break;
				case 19: wiringPiISR(19, INT_EDGE_SETUP, &interruptHandler19); getclock(&tvBegin[19]); break;
				case 20: wiringPiISR(20, INT_EDGE_SETUP, &interruptHandler20); getclock(&tvBegin[20]); break;
				case 21: wiringPiISR(21, INT_EDGE_SETUP, &interruptHandler21); getclock(&tvBegin[21]); break;
				case 22: wiringPiISR(22, INT_EDGE_SETUP, &interruptHandler22); getclock(&tvBegin[22]); break;
				case 23: wiringPiISR(23, INT_EDGE_SETUP, &interruptHandler23); getclock(&tvBegin[23]); break;
				case 24: wiringPiISR(24, INT_EDGE_SETUP, &interruptHandler24); getclock(&tvBegin[24]); break;
				case 25: wiringPiISR(25, INT_EDGE_SETUP, &interruptHandler25); getclock(&tvBegin[25]); break;
				case 26: wiringPiISR(26, INT_EDGE_SETUP, &interruptHandler26); getclock(&tvBegin[26]); break;
				case 27: wiringPiISR(27, INT_EDGE_SETUP, &interruptHandler27); getclock(&tvBegin[27]); break;
				case 28: wiringPiISR(28, INT_EDGE_SETUP, &interruptHandler28); getclock(&tvBegin[28]); break;
				case 29: wiringPiISR(29, INT_EDGE_SETUP, &interruptHandler29); getclock(&tvBegin[29]); break;
				case 30: wiringPiISR(30, INT_EDGE_SETUP, &interruptHandler30); getclock(&tvBegin[30]); break;
				case 31: wiringPiISR(31, INT_EDGE_SETUP, &interruptHandler31); getclock(&tvBegin[31]); break;
				default: _log.Log(LOG_ERROR, "GPIO: Error hooking interrupt handler for unknown GPIO %d.", it->GetId());
			}
		}
	}

	//  Read all exported GPIO ports and set the device status accordingly.
	UpdateDeviceStates(false);

	//  No need for delayed startup and force update when no masters are able to connect.
	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT ID FROM Users WHERE (RemoteSharing==1) AND (Active==1)");
	if (result.size()>0)
	{
		//  Wait 250 milli seconds to make sure all are set before initialising
		//  the remainder of domoticz.
		sleep_milliseconds(250);

		//  Start thread to do a delayed setup of initial state
		m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&CGpio::DelayedStartup, this)));
	}

	if (m_pollinterval > 0)
	{
		m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&CGpio::Poller, this)));
	}

	_log.Log(LOG_NORM, "GPIO: WiringPi is now initialized");
#endif
	sOnConnected(this);

	return (m_thread != NULL);
}


bool CGpio::StopHardware()
{
	if (m_thread != NULL) {
		m_stoprequested=true;
		interruptCondition.notify_one();
		m_thread->join();
	}

	m_bIsStarted=false;

	return true;
}


bool CGpio::WriteToHardware(const char *pdata, const unsigned char length)
{
#ifndef WIN32
	const tRBUF *pCmd = reinterpret_cast<const tRBUF*>(pdata);

	if ((pCmd->LIGHTING1.packettype == pTypeLighting1) && (pCmd->LIGHTING1.subtype == sTypeIMPULS)) {
		unsigned char housecode = (pCmd->LIGHTING1.housecode);
		if (housecode == 0) {
			int gpioId = pCmd->LIGHTING1.unitcode;
			_log.Log(LOG_NORM,"GPIO: WriteToHardware housecode %d, packetlength %d", pCmd->LIGHTING1.housecode, pCmd->LIGHTING1.packetlength);

			int oldValue = digitalRead(gpioId);
			_log.Log(LOG_NORM,"GPIO: pin #%d state was %d", gpioId, oldValue);

			int newValue = static_cast<int>(pCmd->LIGHTING1.cmnd);
			digitalWrite(gpioId, newValue);

			_log.Log(LOG_NORM,"GPIO: WriteToHardware housecode %d, GPIO %d, previously %d, set %d", static_cast<int>(housecode), static_cast<int>(gpioId), oldValue, newValue);
		}
		else {
			_log.Log(LOG_NORM,"GPIO: wrong housecode %d", static_cast<int>(housecode));
			return false;
		}
	}
	else {
		_log.Log(LOG_NORM,"GPIO: WriteToHardware packet type %d or subtype %d unknown", pCmd->LIGHTING1.packettype, pCmd->LIGHTING1.subtype);
		return false;
	}
	return true;
#else
	return false;
#endif
}


void CGpio::ProcessInterrupt(int gpioId) {
#ifndef WIN32
	std::vector<std::vector<std::string> > result;

	result = m_sql.safe_query("SELECT Name,nValue,sValue FROM DeviceStatus WHERE (HardwareID==%d) AND (Unit==%d)", m_HwdID, gpioId);

	if ((!result.empty()) && (result.size() > 0))
	{
		_log.Log(LOG_NORM, "GPIO: Processing interrupt for GPIO %d...", gpioId);

		// Debounce reading
		sleep_milliseconds(m_debounce);

		// Read GPIO data
		int value = digitalRead(gpioId);

		if (value != 0) {
			IOPinStatusPacket.LIGHTING1.cmnd = light1_sOn;
		}
		else {
			IOPinStatusPacket.LIGHTING1.cmnd = light1_sOff;
		}

		unsigned char seqnr = IOPinStatusPacket.LIGHTING1.seqnbr;
		seqnr++;
		IOPinStatusPacket.LIGHTING1.seqnbr = seqnr;
		IOPinStatusPacket.LIGHTING1.unitcode = gpioId;

		sDecodeRXMessage(this, (const unsigned char *)&IOPinStatusPacket, NULL, 255);

		_log.Log(LOG_NORM, "GPIO: Done processing interrupt for GPIO %d (%s).", gpioId, (value != 0) ? "HIGH" : "LOW");
	}
#endif
}

void CGpio::Do_Work()
{
	int interruptNumber = NO_INTERRUPT;
	boost::posix_time::milliseconds duration(12000);
	std::vector<int> triggers;

	_log.Log(LOG_NORM,"GPIO: Worker started, Debounce:%dms Period:%dms Poll-interval:%dsec", m_debounce, m_period, m_pollinterval);

	while (!m_stoprequested) {

		/* housekeeping */
		mytime(&m_LastHeartbeat);

#ifndef WIN32
		
		/* Interrupt handling */
		boost::mutex::scoped_lock lock(interruptQueueMutex);	
		if (interruptCondition.timed_wait(lock, duration)) 
		{
			while (!gpioInterruptQueue.empty()) {
				interruptNumber = gpioInterruptQueue.front();
				triggers.push_back(interruptNumber);
				gpioInterruptQueue.erase(gpioInterruptQueue.begin());
				// _log.Log(LOG_NORM, "GPIO: Acknowledging interrupt for GPIO %d.", interruptNumber);
			}
		}
		lock.unlock();

		if (m_stoprequested) {
			break;
		}

    	while (!triggers.empty()) {
			interruptNumber = triggers.front();
			triggers.erase(triggers.begin());
			CGpio::ProcessInterrupt(interruptNumber);
		}

#else
		sleep_milliseconds(100);
#endif
	}
	_log.Log(LOG_NORM,"GPIO: Worker stopped...");
}


void CGpio::Poller()
{
	//
	//	This code has been added for robustness like for example when gpio is 
	//	used in alarm systems. In case a state change event (interrupt) is 
	//	missed this code will make up for it.
	//
	int sec_counter = 0;

	_log.Log(LOG_STATUS, "GPIO: %d-second poller started", m_pollinterval);

	while (!m_stoprequested)
	{
		sleep_seconds(1);
		sec_counter++;

		if ((sec_counter % m_pollinterval == 0) && (!m_stoprequested))
		{
			UpdateDeviceStates(false);
		}
	}

	_log.Log(LOG_STATUS, "GPIO: %d-second poller stopped", m_pollinterval);
}

/*
 * static
 * One-shot method to initialize pins
 *
 */
bool CGpio::InitPins()
{
	char buf[256];
	bool exports[MAX_GPIO+1] = { false };
	int gpioNumber;
	FILE *cmd = NULL;

	// 1. List exports and parse the result
#ifndef WIN32
	cmd = popen("gpio exports", "r");
#else
	cmd = fopen("E:\\exports.txt", "r");
#endif
	while (fgets(buf, sizeof(buf), cmd) != 0) {
		// Decode GPIO pin number from the output formatted as follows:
		//
		// GPIO Pins exported:
		//   17: out  0  none
		//   18: in   1  none
		//
		// 00000000001111111111
		// 01234567890123456789

		std::string exportLine(buf);
		//	std::cout << "Processing line: " << exportLine;
		std::vector<std::string> sresults;
		StringSplit(exportLine, ":", sresults);
		if (sresults.empty())
			continue;
		//if (sresults[0] == "GPIO")
		if (sresults[0] == "GPIO Pins exported")
			continue;
		std::cout << "results.size: " << sresults.size() << sresults[0];
		//if (sresults.size() >= 4)
		if (sresults.size() >= 2)
		{
			gpioNumber = atoi(sresults[0].c_str());
			if ((gpioNumber >= 0) && (gpioNumber <= MAX_GPIO)) {
				exports[gpioNumber] = true;
			}
			else {
				_log.Log(LOG_NORM, "GPIO: Ignoring unsupported pin '%s'", buf);
			}
		}
	}
#ifndef WIN32
	pclose(cmd);
#else
	fclose(cmd);
#endif

	// 2. List the full pin set and parse the result
#ifndef WIN32
	cmd = popen("gpio readall", "r");
#else
	cmd = fopen("E:\\readall.txt", "r");
#endif
	while (fgets(buf, sizeof(buf), cmd) != 0) {
		// Decode IN and OUT lines from the output formatted as follows:
		//
		// Old style (wiringPi<=2.16):
		// +----------+-Rev1-+------+--------+------+-------+
		// | wiringPi | GPIO | Phys | Name   | Mode | Value |
		// +----------+------+------+--------+------+-------+
		// |      0   |  17  |  11  | GPIO 0 | IN   | Low   |
		// |      1   |  18  |  12  | GPIO 1 | IN   | Low   |
		// |      2   |  21  |  13  | GPIO 2 | IN   | Low   |
		// |      3   |  22  |  15  | GPIO 3 | IN   | Low   |
		// ...
		//
		// New style:
		//  +-----+-----+---------+------+---+--B Plus--+---+------+---------+-----+-----+
		//  | BCM | wPi |   Name  | Mode | V | Physical | V | Mode | Name    | wPi | BCM |
		//  +-----+-----+---------+------+---+----++----+---+------+---------+-----+-----+
		//  |     |     |    3.3v |      |   |  1 || 2  |   |      | 5v      |     |     |
		//  |   2 |   8 |   SDA.1 |   IN | 1 |  3 || 4  |   |      | 5V      |     |     |
		//  |   3 |   9 |   SCL.1 |   IN | 1 |  5 || 6  |   |      | 0v      |     |     |
		//  |   4 |   7 | GPIO. 7 |   IN | 1 |  7 || 8  | 1 | ALT0 | TxD     | 15  | 14  |
		// ...
		//
		// 0000000000111111111122222222223333333333444444444455555555556666666666777777777
		// 0123456789012345678901234567890123456789012345678901234567890123456789012345678
		//
		// ODroid C2
		// +------+-----+----------+------+ Model  ODROID-C2 +------+----------+-----+------+
		// | GPIO | wPi |   Name   | Mode | V | Physical | V | Mode |   Name   | wPi | GPIO |
		// +------+-----+----------+------+---+----++----+---+------+----------+-----+------+
		// |      |     |     3.3v |      |   |  1 || 2  |   |      | 5v       |     |      |
		// |      |   8 |    SDA.1 |      |   |  3 || 4  |   |      | 5V       |     |      |
		// |      |   9 |    SCL.1 |      |   |  5 || 6  |   |      | 0v       |     |      |
		// |  249 |   7 | GPIO.249 |   IN | 1 |  7 || 8  |   |      | TxD1     | 15  |      |
		// ...
		//
		// 0000000000111111111122222222223333333333444444444455555555556666666666777777777788
		// 0123456789012345678901234567890123456789012345678901234567890123456789012345678901

		std::string line(buf);
		std::vector<std::string> fields;

		//std::cout << "Processing line: " << line;
		StringSplit(line, "|", fields);
		if (fields.size()<7)
			continue;

		//std::cout << "# fields: " << fields.size() << std::endl;

		// trim each field
		for (size_t i = 0; i < fields.size(); i++) {
			fields[i]=stdstring_trim(fields[i]);
			//std::cout << "fields[" << i << "] = '" << fields[i] << "'" << std::endl;
		}

		if (fields.size() == 7) {
			// Old style
			if (fields[0] != "wiringPi") {
				gpioNumber = atoi(fields[1].c_str());
				if ((gpioNumber >= 0) && (gpioNumber <= MAX_GPIO)) {
					pins.push_back(CGpioPin(gpioNumber, "gpio" + fields[1] + " (" + fields[3] + ") on pin " + fields[2],
							fields[4] == "IN", fields[4] == "OUT", exports[gpioNumber]));
				} else {
					_log.Log(LOG_NORM, "GPIO: Ignoring unsupported pin '%s'", fields[1].c_str());
				}
			}
		} else if (fields.size() == 15) {
			// New style
			if (fields[1].length() > 0) {
				gpioNumber = atoi(fields[1].c_str());
				if ((gpioNumber >= 0) && (gpioNumber <= MAX_GPIO)) {
					pins.push_back(CGpioPin(gpioNumber, "gpio" + fields[1] + " (" + fields[3] + ") on pin " + fields[6],
							(fields[4] == "IN"), (fields[4] == "OUT"), exports[gpioNumber]));
				} else {
					_log.Log(LOG_NORM, "GPIO: Ignoring unsupported pin '%s'", fields[1].c_str());
				}
			}

			if (fields[13].length() > 0) {
				gpioNumber = atoi(fields[13].c_str());
				if ((gpioNumber >= 0) && (gpioNumber <= MAX_GPIO)) {
					pins.push_back(CGpioPin(gpioNumber, "gpio" + fields[13] + " (" + fields[11] + ") on pin " + fields[8],
							fields[10] == "IN", fields[10] == "OUT", exports[gpioNumber]));
				} else {
					_log.Log(LOG_NORM, "GPIO: Ignoring unsupported pin '%s'", fields[13].c_str());
				}
			}
		}
	}
#ifndef WIN32
	pclose(cmd);
#else
	fclose(cmd);
#endif
	if (pins.size() > 0) {
		std::sort(pins.begin(), pins.end());
		// debug
		//for(std::vector<CGpioPin>::iterator it = pins.begin(); it != pins.end(); ++it) {
		//	CGpioPin pin=*it;
		//	std::cout << "Pin " << pin.GetId() << " : " << pin.GetLabel() << ", " << pin.GetIsInput() << ", " << pin.GetIsOutput() << ", " << pin.GetIsExported() << std::endl;
		//}
	} else {
		_log.Log(LOG_ERROR, "GPIO: Failed to detect any pins, make sure you exported them!");
		return false;
	}
	return true;
}

/* static */
std::vector<CGpioPin> CGpio::GetPinList()
{
	return pins;
}

/* static */
CGpioPin* CGpio::GetPPinById(int id)
{
	for(std::vector<CGpioPin>::iterator it = pins.begin(); it != pins.end(); ++it) {
		if (it->GetId() == id) {
			return &(*it);
		}
	}
	return NULL;
}

void CGpio::DelayedStartup()
{
	//
	//	This runs to better support Raspberry Pi as domoticz slave device.
	//
	//  Delay 30 seconds to make sure master domoticz has connected. Then
	//	copy the GPIO ports states to the switches one more time so the
	//	master will see the actual states also after it has connected.
	//
	sleep_milliseconds(30000);
	_log.Log(LOG_NORM, "GPIO: Optional connected Master Domoticz now updates its status");
	UpdateDeviceStates(true);

	// _log.Log(LOG_NORM, "GPIO: DelayedStartup - done");
}

void CGpio::UpdateDeviceStates(bool forceUpdate)
{
    char buf[256];
    int gpioId;
    FILE *cmd = NULL;

    cmd = popen("gpio exports", "r");

    while (fgets(buf, sizeof(buf), cmd) != 0)
    {
		//
        // Decode GPIO pin number from the output formatted as follows:
        // GPIO Pins exported: "18: in 27 both"
        //
        std::string exportLine(buf);
        std::vector<std::string> sresults;
        StringSplit(exportLine, ":", sresults);

        if (sresults.empty())
        {
			continue;
        }

        if (sresults[0] == "GPIO Pins exported")
        {
			continue;
        }

        if (sresults.size() >= 2)
        {
			gpioId = atoi(sresults[0].c_str());

			if ((gpioId >= 0) && (gpioId <= MAX_GPIO))
			{
				UpdateState(gpioId, forceUpdate);
			}
			else
			{
				_log.Log(LOG_NORM, "GPIO: UpdateDeviceStates - Ignoring unsupported pin '%s'", buf);
			}
        }
    }
}

void CGpio::UpdateState(int gpioId, bool forceUpdate)
{
	bool updateDatabase = false;
	int state = digitalRead(gpioId);
	std::vector<std::vector<std::string> > result;

	result = m_sql.safe_query("SELECT Name,nValue,sValue FROM DeviceStatus WHERE (HardwareID==%d) AND (Unit==%d)", m_HwdID, gpioId);

	if ((!result.empty()) && (result.size()>0))
	{
		if ((!result.empty()) && (result.size()>0))
		{
			std::vector<std::string> sd=result[0];

			int dbaseState = atoi(sd[1].c_str());

			if ((dbaseState != state) || (forceUpdate))
			{
				updateDatabase = true;
			}
		}
	}

	if (updateDatabase)
	{
		if (state != 0)
		{
			IOPinStatusPacket.LIGHTING1.cmnd = light1_sOn;
		}
		else
		{
			IOPinStatusPacket.LIGHTING1.cmnd = light1_sOff;
		}

		unsigned char seqnr = IOPinStatusPacket.LIGHTING1.seqnbr;
		seqnr++;
		IOPinStatusPacket.LIGHTING1.seqnbr = seqnr;
		IOPinStatusPacket.LIGHTING1.unitcode = gpioId;

		sDecodeRXMessage(this, (const unsigned char *)&IOPinStatusPacket, NULL, 255);
	}

	//  _log.Log(LOG_NORM, "GPIO:%d initial state %s", gpioId, (value != 0) ? "OPEN" : "CLOSED");
}

#endif // WITH_GPIO

