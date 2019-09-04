/*
 * Platform.cpp
 *
 *  Created on: 9 Sep 2018
 *      Author: David
 */

#include "Platform.h"

#include <Hardware/IoPorts.h>
#include <Hardware/AnalogIn.h>
#include <Hardware/AnalogOut.h>
#include <Movement/Move.h>
#include "Movement/StepperDrivers/TMC51xx.h"
#include "Movement/StepperDrivers/TMC22xx.h"
#include <atmel_start.h>
#include <Config/peripheral_clk_config.h>
#include "AdcAveragingFilter.h"
#include "Movement/StepTimer.h"
#include <CAN/CanInterface.h>
#include "Tasks.h"
#include "Heating/Heat.h"
#include "Heating/Sensors/TemperatureSensor.h"
#include "Fans/FansManager.h"

#if defined(SAME51)

# include <hri_nvmctrl_e51.h>
constexpr uint32_t FlashBlockSize = 0x00010000;							// the block size we assume for flash
constexpr uint32_t FirmwareFlashStart = FLASH_ADDR + FlashBlockSize;	// we reserve 64K for the bootloader

#elif defined(SAMC21)

# include <hri_nvmctrl_c21.h>
constexpr uint32_t FlashBlockSize = 0x00004000;							// the block size we assume for flash
constexpr uint32_t FirmwareFlashStart = FLASH_ADDR + FlashBlockSize;	// we reserve 16K for the bootloader

#else
# error Unsupported processor
#endif

static bool txBusy = false;
static bool doFirmwareUpdate = false;

extern "C" void tx_cb_USART_0(const struct usart_async_descriptor *const io_descr)
{
	txBusy = false;
}

namespace Platform
{
	static constexpr float DefaultStepsPerMm = 80.0;

	Mutex messageMutex;

	static struct io_descriptor *io;

	static uint32_t errorCodeBits = 0;
	uint32_t slowDriversBitmap = 0;
	uint32_t slowDriverStepTimingClocks[4] = { 0, 0, 0, 0 };

	uint32_t driveDriverBits[NumDrivers];
	uint32_t allDriverBits = 0;

	static bool directions[NumDrivers];
	static int8_t enableValues[NumDrivers] = { 0 };
	static float stepsPerMm[NumDrivers];

	static volatile uint16_t currentVin, highestVin, lowestVin;
#if HAS_12V_MONITOR
	static volatile uint16_t currentV12, highestV12, lowestV12;
#endif

//	static uint16_t lastUnderVoltageValue, lastOverVoltageValue;
	static uint32_t numUnderVoltageEvents, previousUnderVoltageEvents;
	static volatile uint32_t numOverVoltageEvents, previousOverVoltageEvents;

	static float currentMcuTemperature, highestMcuTemperature, lowestMcuTemperature;
	static float mcuTemperatureAdjust = 0.0;

	static uint32_t lastPollTime;
	static uint32_t lastFanCheckTime = 0;
	static unsigned int heatTaskIdleTicks = 0;

	// SERCOM3 Rx is on PB21 (OUT_8_TACHO), Tx is on PB20 (OUT_7_TACHO)

	static ThermistorAveragingFilter thermistorFilters[NumThermistorFilters];
	static AdcAveragingFilter<VinReadingsAveraged> vinFilter;
#if HAS_12V_MONITOR
	static AdcAveragingFilter<VinReadingsAveraged> v12Filter;
#endif

#if defined(SAME51)
	static AdcAveragingFilter<McuTempReadingsAveraged> tpFilter;
	static AdcAveragingFilter<McuTempReadingsAveraged> tcFilter;
#elif defined(SAMC21)
	static AdcAveragingFilter<McuTempReadingsAveraged> tsensFilter;
#else
# error Unsupported processor
#endif

#if HAS_SMART_DRIVERS
	static DriversBitmap temperatureShutdownDrivers, temperatureWarningDrivers, shortToGroundDrivers;
	static DriversBitmap openLoadADrivers, openLoadBDrivers, notOpenLoadADrivers, notOpenLoadBDrivers;
	MillisTimer openLoadATimer, openLoadBTimer;
	MillisTimer driversFanTimer;		// driver cooling fan timer
	static uint8_t nextDriveToPoll;
#endif

#if HAS_SMART_DRIVERS && HAS_VOLTAGE_MONITOR
	bool warnDriversNotPowered;
#endif

#if HAS_STALL_DETECT
	DriversBitmap logOnStallDrivers, pauseOnStallDrivers, rehomeOnStallDrivers;
	DriversBitmap stalledDrivers, stalledDriversToLog, stalledDriversToPause, stalledDriversToRehome;
#endif

#ifdef SAME51
	static int32_t tempCalF1, tempCalF2, tempCalF3, tempCalF4;		// temperature calibration factors

	static void ADC_temperature_init(void)
	{
		// Temperature sense stuff
		constexpr uint32_t NVM_TEMP_CAL_TLI_POS = 0;
		constexpr uint32_t NVM_TEMP_CAL_TLI_SIZE = 8;
		constexpr uint32_t NVM_TEMP_CAL_TLD_POS = 8;
		constexpr uint32_t NVM_TEMP_CAL_TLD_SIZE = 4;
		constexpr uint32_t NVM_TEMP_CAL_THI_POS = 12;
		constexpr uint32_t NVM_TEMP_CAL_THI_SIZE = 8;
		constexpr uint32_t NVM_TEMP_CAL_THD_POS = 20;
		constexpr uint32_t NVM_TEMP_CAL_THD_SIZE = 4;
		constexpr uint32_t NVM_TEMP_CAL_VPL_POS = 40;
		constexpr uint32_t NVM_TEMP_CAL_VPL_SIZE = 12;
		constexpr uint32_t NVM_TEMP_CAL_VPH_POS = 52;
		constexpr uint32_t NVM_TEMP_CAL_VPH_SIZE = 12;
		constexpr uint32_t NVM_TEMP_CAL_VCL_POS = 64;
		constexpr uint32_t NVM_TEMP_CAL_VCL_SIZE = 12;
		constexpr uint32_t NVM_TEMP_CAL_VCH_POS = 76;
		constexpr uint32_t NVM_TEMP_CAL_VCH_SIZE = 12;

		const uint16_t temp_cal_vpl = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_VPL_POS / 32)) >> (NVM_TEMP_CAL_VPL_POS % 32))
		               & ((1u << NVM_TEMP_CAL_VPL_SIZE) - 1);
		const uint16_t temp_cal_vph = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_VPH_POS / 32)) >> (NVM_TEMP_CAL_VPH_POS % 32))
		               & ((1u << NVM_TEMP_CAL_VPH_SIZE) - 1);
		const uint16_t temp_cal_vcl = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_VCL_POS / 32)) >> (NVM_TEMP_CAL_VCL_POS % 32))
		               & ((1u << NVM_TEMP_CAL_VCL_SIZE) - 1);
		const uint16_t temp_cal_vch = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_VCH_POS / 32)) >> (NVM_TEMP_CAL_VCH_POS % 32))
		               & ((1u << NVM_TEMP_CAL_VCH_SIZE) - 1);

		const uint8_t temp_cal_tli = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_TLI_POS / 32)) >> (NVM_TEMP_CAL_TLI_POS % 32))
		               & ((1u << NVM_TEMP_CAL_TLI_SIZE) - 1);
		const uint8_t temp_cal_tld = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_TLD_POS / 32)) >> (NVM_TEMP_CAL_TLD_POS % 32))
		               & ((1u << NVM_TEMP_CAL_TLD_SIZE) - 1);
		const uint16_t temp_cal_tl = ((uint16_t)temp_cal_tli) << 4 | ((uint16_t)temp_cal_tld);

		const uint8_t temp_cal_thi = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_THI_POS / 32)) >> (NVM_TEMP_CAL_THI_POS % 32))
		               & ((1u << NVM_TEMP_CAL_THI_SIZE) - 1);
		const uint8_t temp_cal_thd = (*((uint32_t *)(NVMCTRL_TEMP_LOG) + (NVM_TEMP_CAL_THD_POS / 32)) >> (NVM_TEMP_CAL_THD_POS % 32))
		               & ((1u << NVM_TEMP_CAL_THD_SIZE) - 1);
		const uint16_t temp_cal_th = ((uint16_t)temp_cal_thi) << 4 | ((uint16_t)temp_cal_thd);

		tempCalF1 = (int32_t)temp_cal_tl * (int32_t)temp_cal_vph - (int32_t)temp_cal_th * (int32_t)temp_cal_vpl;
		tempCalF2 = (int32_t)temp_cal_tl * (int32_t)temp_cal_vch - (int32_t)temp_cal_th * (int32_t)temp_cal_vcl;
		tempCalF3 = (int32_t)temp_cal_vcl - (int32_t)temp_cal_vch;
		tempCalF4 = (int32_t)temp_cal_vpl - (int32_t)temp_cal_vph;
	}
#endif

	// Send the specified message to the specified destinations. The Error and Warning flags have already been handled.
	void RawMessage(MessageType type, const char *message)
	{
		// io_write requires that the message doesn't go out of scope until transmission is complete, so copy it to a static buffer
		static String<200> buffer;

		MutexLocker lock(messageMutex);

		buffer.copy("{\"message\":\"");
		buffer.cat(message);		// should do JSON escaping here
		buffer.cat("\"}\n");
		txBusy = true;
		io_write(io, (const unsigned char *)buffer.c_str(), buffer.strlen());
		//while (txBusy) { delay(1); }
	}

#ifdef SAME51
	// Set a contiguous range of interrupts to the specified priority
	static void SetInterruptPriority(IRQn base, unsigned int num, uint32_t prio)
	{
		do
		{
			NVIC_SetPriority(base, prio);
			base = (IRQn)(base + 1);
			--num;
		}
		while (num != 0);
	}
#endif

	static void InitialiseInterrupts()
	{
#ifdef SAME51
		// Set UART interrupt priority. Each SERCOM has up to 4 interrupts, numbered sequentially.
		SetInterruptPriority(Serial0_IRQn, 4, NvicPriorityUart);
		SetInterruptPriority(Serial1_IRQn, 4, NvicPriorityUart);
#endif

		NVIC_SetPriority(CAN1_IRQn, NvicPriorityCan);
		NVIC_SetPriority(StepTcIRQn, NvicPriorityStep);

#if defined(SAME51)
		SetInterruptPriority(DMAC_0_IRQn, 5, NvicPriorityDmac);
		SetInterruptPriority(EIC_0_IRQn, 16, NvicPriorityPins);
#elif defined(SAMC21)
		NVIC_SetPriority(DMAC_IRQn, NvicPriorityDmac);
		NVIC_SetPriority(EIC_IRQn, NvicPriorityPins);
#else
# error Undefined processor
#endif

		StepTimer::Init();										// initialise the step pulse timer
	}

	[[noreturn]] RAMFUNC static void EraseAndReset()
	{
#if defined(SAME51)
		while (!hri_nvmctrl_get_STATUS_READY_bit(NVMCTRL)) { }

		// Unlock the block of flash
		hri_nvmctrl_write_ADDR_reg(NVMCTRL, FirmwareFlashStart);
		hri_nvmctrl_write_CTRLB_reg(NVMCTRL, NVMCTRL_CTRLB_CMD_UR | NVMCTRL_CTRLB_CMDEX_KEY);

		while (!hri_nvmctrl_get_STATUS_READY_bit(NVMCTRL)) { }

		// Set address and command
		hri_nvmctrl_write_ADDR_reg(NVMCTRL, FirmwareFlashStart);
		hri_nvmctrl_write_CTRLB_reg(NVMCTRL, NVMCTRL_CTRLB_CMD_EB | NVMCTRL_CTRLB_CMDEX_KEY);

		while (!hri_nvmctrl_get_STATUS_READY_bit(NVMCTRL)) { }
#elif defined(SAMC21)
		while (!hri_nvmctrl_get_interrupt_READY_bit(NVMCTRL)) { }
		hri_nvmctrl_clear_STATUS_reg(NVMCTRL, NVMCTRL_STATUS_MASK);

		// Unlock the block of flash
		hri_nvmctrl_write_ADDR_reg(NVMCTRL, FirmwareFlashStart / 2);		// note the /2 because the command takes the address in 16-bit words
		hri_nvmctrl_write_CTRLA_reg(NVMCTRL, NVMCTRL_CTRLA_CMD_UR | NVMCTRL_CTRLA_CMDEX_KEY);

		while (!hri_nvmctrl_get_interrupt_READY_bit(NVMCTRL)) { }
		hri_nvmctrl_clear_STATUS_reg(NVMCTRL, NVMCTRL_STATUS_MASK);

		// Set address and command
		hri_nvmctrl_write_ADDR_reg(NVMCTRL, FirmwareFlashStart / 2);		// note the /2 because the command takes the address in 16-bit words
		hri_nvmctrl_write_CTRLA_reg(NVMCTRL, NVMCTRL_CTRLA_CMD_ER | NVMCTRL_CTRLA_CMDEX_KEY);

		while (!hri_nvmctrl_get_interrupt_READY_bit(NVMCTRL)) { }
		hri_nvmctrl_clear_STATUS_reg(NVMCTRL, NVMCTRL_STATUS_MASK);
#else
# error Unsupported processor
#endif

		SCB->AIRCR = (0x5FA << 16) | (1u << 2);			// reset the processor
		for (;;) { }
	}

	[[noreturn]] static void DoFirmwareUpdate()
	{
		CanInterface::Shutdown();
		DisableAllDrives();
		delay(10);										// allow existing processing to complete, drivers to be turned off and CAN replies to be sent
		Heat::SwitchOffAll();
#ifdef SAME51
		IoPort::WriteDigital(GlobalTmc51xxEnablePin, true);
#endif
#ifdef SAMC21
		IoPort::WriteDigital(GlobalTmc22xxEnablePin, true);
#endif

//		DisableCache();

		// Disable all IRQs
		__disable_irq();
		SysTick->CTRL = (1 << SysTick_CTRL_CLKSOURCE_Pos);	// disable the system tick exception

#if defined(SAME51)
		for (size_t i = 0; i < 8; i++)
		{
			NVIC->ICER[i] = 0xFFFFFFFF;					// Disable IRQs
			NVIC->ICPR[i] = 0xFFFFFFFF;					// Clear pending IRQs
		}
#elif defined(SAMC21)
		NVIC->ICER[0] = 0xFFFFFFFF;						// Disable IRQs
		NVIC->ICPR[0] = 0xFFFFFFFF;						// Clear pending IRQs
#else
# error Unsupported processor
#endif

		digitalWrite(DiagLedPin, false);					// turn the DIAG LED off

		EraseAndReset();
	}

}	// end namespace Platform

void Platform::Init()
{
	IoPort::Init();

	// Set up the DIAG LED pin
	IoPort::SetPinMode(DiagLedPin, OUTPUT_HIGH);

	messageMutex.Create("Message");

	// Turn all outputs off
	for (size_t pin = 0; pin < ARRAY_SIZE(PinTable); ++pin)
	{
		const PinDescription& p = PinTable[pin];
		if (p.pinNames != nullptr)
		{
			if (   StringStartsWith(p.pinNames, "out")
				&& strlen(p.pinNames) < 5							// don't set "outN.tach" pins to outputs
		      )
			{
				IoPort::SetPinMode(pin, OUTPUT_LOW);				// turn off heaters and fans (although this will turn on PWM fans)
			}
			else if (StringStartsWith(p.pinNames, "spi.cs"))
			{
				IoPort::SetPinMode(pin, INPUT_PULLUP);				// ensure SPI CS lines are high so that temp daughter boards don't drive the bus before they are configured
			}
		}
	}

	// Set up the UART to send to PanelDue
	//TODO finish our own serial driver
#if defined(SAME51)
	const uint32_t baudDiv = 65536 - ((65536 * 16.0f * 57600) / CONF_GCLK_SERCOM3_CORE_FREQUENCY);
	usart_async_set_baud_rate(&USART_0, baudDiv);

	usart_async_register_callback(&USART_0, USART_ASYNC_TXC_CB, tx_cb_USART_0);
	//usart_async_register_callback(&USART_0, USART_ASYNC_RXC_CB, rx_cb);
	//usart_async_register_callback(&USART_0, USART_ASYNC_ERROR_CB, err_cb);
	usart_async_get_io_descriptor(&USART_0, &io);
	usart_async_enable(&USART_0);
#elif defined(SAMC21)
	//TODO
#else
# error Unsupported processor
#endif

	// Initialise the rest of the IO subsystem
	AnalogIn::Init();
	AnalogOut::Init();

#ifdef SAME51
	ADC_temperature_init();
#endif

#ifdef SAME51		//TODO base on configuration not processor
	// Set up the board ID switch inputs
	for (unsigned int i = 0; i < 4; ++i)
	{
		IoPort::SetPinMode(BoardAddressPins[i], INPUT_PULLUP);
	}
#endif

	// Set up VIN voltage monitoring
	currentVin = highestVin = 0;
	lowestVin = 9999;
	numUnderVoltageEvents = previousUnderVoltageEvents = numOverVoltageEvents = previousOverVoltageEvents = 0;

	vinFilter.Init(0);
	AnalogIn::EnableChannel(VinMonitorPin, vinFilter.CallbackFeedIntoFilter, &vinFilter);

#if HAS_12V_MONITOR
	currentV12 = highestV12 = 0;
	lowestV12 = 9999;

	v12Filter.Init(0);
	AnalogIn::EnableChannel(V12MonitorPin, v12Filter.CallbackFeedIntoFilter, &v12Filter);
#endif

#if HAS_VREF_MONITOR
	thermistorFilters[VrefFilterIndex].Init(0);
	AnalogIn::EnableChannel(VrefPin, thermistorFilters[VrefFilterIndex].CallbackFeedIntoFilter, &thermistorFilters[VrefFilterIndex]);
	thermistorFilters[VssaFilterIndex].Init(0);
	AnalogIn::EnableChannel(VssaPin, thermistorFilters[VssaFilterIndex].CallbackFeedIntoFilter, &thermistorFilters[VssaFilterIndex]);
#endif

	// Set up the thermistor filters
	for (size_t i = 0; i < NumThermistorInputs; ++i)
	{
		thermistorFilters[i].Init(0);
		AnalogIn::EnableChannel(TempSensePins[i], thermistorFilters[i].CallbackFeedIntoFilter, &thermistorFilters[i]);
	}

	// Set up the MCU temperature sensors
	currentMcuTemperature = 0.0;
	highestMcuTemperature = -273.16;
	lowestMcuTemperature = 999.0;
	mcuTemperatureAdjust = 0.0;

#if defined(SAME51)
	tpFilter.Init(0);
	AnalogIn::EnableTemperatureSensor(0, tpFilter.CallbackFeedIntoFilter, &tpFilter, 1, 0);
	tcFilter.Init(0);
	AnalogIn::EnableTemperatureSensor(1, tcFilter.CallbackFeedIntoFilter, &tcFilter, 1, 0);
#elif defined(SAMC21)
	tsensFilter.Init(0);
	AnalogIn::EnableTemperatureSensor(tsensFilter.CallbackFeedIntoFilter, &tsensFilter, 1);
#else
# error Unsupported processor
#endif

	// Initialise stepper drivers
	SmartDrivers::Init();
	temperatureShutdownDrivers = temperatureWarningDrivers = shortToGroundDrivers = openLoadADrivers = openLoadBDrivers = notOpenLoadADrivers = notOpenLoadBDrivers = 0;

	for (size_t i = 0; i < NumDrivers; ++i)
	{
		IoPort::SetPinMode(StepPins[i], OUTPUT_LOW);
		IoPort::SetPinMode(DirectionPins[i], OUTPUT_LOW);
		const uint32_t driverBit = 1u << (StepPins[i] & 31);
		driveDriverBits[i] = driverBit;
		allDriverBits |= driverBit;
		stepsPerMm[i] = DefaultStepsPerMm;
		directions[i] = true;

		SmartDrivers::SetMicrostepping(i, 16, true);
	}

#if HAS_STALL_DETECT
	stalledDrivers = 0;
	logOnStallDrivers = pauseOnStallDrivers = rehomeOnStallDrivers = 0;
	stalledDriversToLog = stalledDriversToPause = stalledDriversToRehome = 0;
#endif

#if HAS_VOLTAGE_MONITOR
	autoSaveEnabled = false;
	autoSaveState = AutoSaveState::starting;
#endif

#if HAS_SMART_DRIVERS && HAS_VOLTAGE_MONITOR
	warnDriversNotPowered = false;
#endif

	CanInterface::Init(ReadBoardId());

	InitialiseInterrupts();

	lastPollTime = millis();

#if 0
	//TEST set up some PWM values
	static PwmPort out0, out1, out2, out3, out4, out5, out6, out7, out8;
	String<100> reply;

	out0.AssignPort("out0", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out1.AssignPort("out1", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out2.AssignPort("out2", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out3.AssignPort("out3", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out4.AssignPort("out4", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out5.AssignPort("out5", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out6.AssignPort("out6", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out7.AssignPort("out7", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);
	out8.AssignPort("out8", reply.GetRef(), PinUsedBy::gpio, PinAccess::pwm);

	out0.SetFrequency(100);
	out0.WriteAnalog(0.1);

	out1.SetFrequency(200);
	out1.WriteAnalog(0.2);

	out2.SetFrequency(400);
	out2.WriteAnalog(0.3);

	out3.SetFrequency(500);
	out3.WriteAnalog(0.4);

	out4.SetFrequency(1000);
	out4.WriteAnalog(0.5);

	out5.SetFrequency(2000);
	out5.WriteAnalog(0.6);

	out6.SetFrequency(4000);
	out6.WriteAnalog(0.7);

	out7.SetFrequency(10000);
	out7.WriteAnalog(0.8);

	out8.SetFrequency(25000);
	out8.WriteAnalog(0.9);
#endif
}

void Platform::Spin()
{
	static bool powered = false;

	if (doFirmwareUpdate)
	{
		DoFirmwareUpdate();
	}

	// Get the VIN voltage
	const float voltsVin = (vinFilter.GetSum() * (3.3 * VinDividerRatio))/(4096 * vinFilter.NumAveraged());
#if HAS_12V_MONITOR
	const float volts12 = (v12Filter.GetSum() * (3.3 * V12DividerRatio))/(4096 * v12Filter.NumAveraged());
	if (!powered && voltsVin >= 10.5 && volts12 >= 10.5)
	{
		powered = true;
	}
	else if (powered && (voltsVin < 10.0 || volts12 < 10.0))
	{
		powered = false;
		++numUnderVoltageEvents;
	}
#else
	if (!powered && voltsVin >= 10.5)
	{
		powered = true;
	}
	else if (powered && voltsVin < 10.0)
	{
		powered = false;
	}
#endif

	SmartDrivers::Spin(powered);

	// Thermostatically-controlled fans (do this after getting TMC driver status)
	const uint32_t now = millis();
	if (now - lastFanCheckTime >= FanCheckInterval)
	{
		(void)FansManager::CheckFans();

		// Check one TMC driver for temperature warning or temperature shutdown
		if (enableValues[nextDriveToPoll] >= 0)				// don't poll driver if it is flagged "no poll"
		{
			const uint32_t stat = SmartDrivers::GetAccumulatedStatus(nextDriveToPoll, 0);
			const DriversBitmap mask = MakeBitmap<DriversBitmap>(nextDriveToPoll);
			if (stat & TMC_RR_OT)
			{
				temperatureShutdownDrivers |= mask;
			}
			else if (stat & TMC_RR_OTPW)
			{
				temperatureWarningDrivers |= mask;
			}
			if (stat & TMC_RR_S2G)
			{
				shortToGroundDrivers |= mask;
			}
			else
			{
				shortToGroundDrivers &= ~mask;
			}

			// The driver often produces a transient open-load error, especially in stealthchop mode, so we require the condition to persist before we report it.
			// Also, false open load indications persist when in standstill, if the phase has zero current in that position
			if ((stat & TMC_RR_OLA) != 0)
			{
				if (!openLoadATimer.IsRunning())
				{
					openLoadATimer.Start();
					openLoadADrivers = notOpenLoadADrivers = 0;
				}
				openLoadADrivers |= mask;
			}
			else if (openLoadATimer.IsRunning())
			{
				notOpenLoadADrivers |= mask;
				if ((openLoadADrivers & ~notOpenLoadADrivers) == 0)
				{
					openLoadATimer.Stop();
				}
			}

			if ((stat & TMC_RR_OLB) != 0)
			{
				if (!openLoadBTimer.IsRunning())
				{
					openLoadBTimer.Start();
					openLoadBDrivers = notOpenLoadBDrivers = 0;
				}
				openLoadBDrivers |= mask;
			}
			else if (openLoadBTimer.IsRunning())
			{
				notOpenLoadBDrivers |= mask;
				if ((openLoadBDrivers & ~notOpenLoadBDrivers) == 0)
				{
					openLoadBTimer.Stop();
				}
			}

# if HAS_STALL_DETECT
			if ((stat & TMC_RR_SG) != 0)
			{
				if ((stalledDrivers & mask) == 0)
				{
					// This stall is new so check whether we need to perform some action in response to the stall
					if ((rehomeOnStallDrivers & mask) != 0)
					{
						stalledDriversToRehome |= mask;
					}
					else if ((pauseOnStallDrivers & mask) != 0)
					{
						stalledDriversToPause |= mask;
					}
					else if ((logOnStallDrivers & mask) != 0)
					{
						stalledDriversToLog |= mask;
					}
				}
				stalledDrivers |= mask;
			}
			else
			{
				stalledDrivers &= ~mask;
			}
# endif
		}

# if 0 //HAS_STALL_DETECT
		// Action any pause or rehome actions due to motor stalls. This may have to be done more than once.
		if (stalledDriversToRehome != 0)
		{
			if (reprap.GetGCodes().ReHomeOnStall(stalledDriversToRehome))
			{
				stalledDriversToRehome = 0;
			}
		}
		else if (stalledDriversToPause != 0)
		{
			if (reprap.GetGCodes().PauseOnStall(stalledDriversToPause))
			{
				stalledDriversToPause = 0;
			}
		}
# endif
		// Advance drive number ready for next time
		++nextDriveToPoll;
		if (nextDriveToPoll == MaxSmartDrivers)
		{
			nextDriveToPoll = 0;
		}
	}

	// Update the Diag LED. Flash it quickly (8Hz) if we are not synced to the master, else flash in sync with the master (about 2Hz).
	gpio_set_pin_level(DiagLedPin,
						(StepTimer::IsSynced()) ? (StepTimer::GetMasterTime() & (1u << 19)) != 0
							: (StepTimer::GetInterruptClocks() & (1u << 17)) != 0
					  );

	if (now - lastPollTime > 2000)
	{
		lastPollTime = now;

#if 0
		static uint8_t oldAddr = 0xFF;
		const uint8_t addr = ReadBoardId();
		if (addr != oldAddr)
		{
			oldAddr = addr;
			const float current = (addr == 3) ? 6300.0 : (addr == 2) ? 2000.0 : (addr == 1) ? 1000.0 : 500.0;
			for (size_t i = 0; i < NumDrivers; ++i)
			{
				SmartDrivers::SetCurrent(i, current);
			}
		}
#endif

		// Get the chip temperature
#if defined(SAME51)
		if (tcFilter.IsValid() && tpFilter.IsValid())
		{
			// From the datasheet:
			// T = (tl * vph * tc - th * vph * tc - tl * tp *vch + th * tp * vcl)/(tp * vcl - tp * vch - tc * vpl * tc * vph)
			const uint16_t tc_result = tcFilter.GetSum()/tcFilter.NumAveraged();
			const uint16_t tp_result = tpFilter.GetSum()/tpFilter.NumAveraged();

			int32_t result =  (tempCalF1 * tc_result - tempCalF2 * tp_result);
			const int32_t divisor = (tempCalF3 * tp_result - tempCalF4 * tc_result);
			result = (divisor == 0) ? 0 : result/divisor;
			currentMcuTemperature = (float)result/16 + mcuTemperatureAdjust;
#elif defined(SAMC21)
		if (tsensFilter.IsValid())
		{
			const int32_t temperatureTimes100 = (int32_t)tsensFilter.GetSum()/tsensFilter.NumAveraged() - (int32_t)(1u << 23);
			currentMcuTemperature = (float)temperatureTimes100 * 0.01;
#else
# error Unsupported processor
#endif
			if (currentMcuTemperature < lowestMcuTemperature)
			{
				lowestMcuTemperature = currentMcuTemperature;
			}
			if (currentMcuTemperature > highestMcuTemperature)
			{
				highestMcuTemperature = currentMcuTemperature;
			}
		}

		static unsigned int nextSensor = 0;

		TemperatureSensor * const ts = Heat::GetSensorAtOrAbove(nextSensor);
		if (ts != nullptr)
		{
			float temp;
			const TemperatureError err = ts->GetLatestTemperature(temp);
			debugPrintf("Sensor %u err %u temp %.1f", ts->GetSensorNumber(), (unsigned int)err, (double)temp);
			nextSensor = ts->GetSensorNumber() + 1;
		}
		else
		{
			nextSensor = 0;
#if 0
			String<100> status;
			SmartDrivers::AppendDriverStatus(2, status.GetRef());
			debugPrintf("%s", status.c_str());
#elif 0
			moveInstance->Diagnostics(AuxMessage);
#elif 0
#else
//			uint32_t conversionsStarted, conversionsCompleted;
//			AnalogIn::GetDebugInfo(conversionsStarted, conversionsCompleted);
			debugPrintf(
//							"Conv %u %u"
						"Addr %u"
#if HAS_12V_MONITOR
						" %.1fV %.1fV"
#else
						" %.1fV"
#endif
						" %.1fC"
						" %u %u"
//						", ptat %d, ctat %d"
						", stat %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
//							(unsigned int)conversionsStarted, (unsigned int)conversionsCompleted,
//							StepTimer::GetInterruptClocks(),
						(unsigned int)CanInterface::GetCanAddress(),
#if HAS_12V_MONITOR
						(double)voltsVin, (double)volts12,
#else
						(double)voltsVin,
#endif
						(double)currentMcuTemperature
						, (unsigned int)thermistorFilters[VrefFilterIndex].GetSum(), (unsigned int)thermistorFilters[VssaFilterIndex].GetSum()
//						, tp_result, tc_result
						, SmartDrivers::GetAccumulatedStatus(0, 0), SmartDrivers::GetAccumulatedStatus(1, 0), SmartDrivers::GetAccumulatedStatus(2, 0)
//							, SmartDrivers::GetLiveStatus(0), SmartDrivers::GetLiveStatus(1), SmartDrivers::GetLiveStatus(2)
					   );
#endif
		}
	}
}

// Get the index of the averaging filter for an analog port
int Platform::GetAveragingFilterIndex(const IoPort& port)
{
	for (size_t i = 0; i < NumThermistorFilters; ++i)
	{
		if (port.GetPin() == TempSensePins[i])
		{
			return (int)i;
		}
	}
	return -1;
}


ThermistorAveragingFilter& Platform::GetAdcFilter(unsigned int filterNumber)
{
	return thermistorFilters[filterNumber];
}

void Platform::GetMcuTemperatures(float& minTemp, float& currentTemp, float& maxTemp)
{
	minTemp = lowestMcuTemperature;
	currentTemp = currentMcuTemperature;
	maxTemp = highestMcuTemperature;
}

void Platform::KickHeatTaskWatchdog()
{
	heatTaskIdleTicks = 0;
}

void Platform::HandleHeaterFault(unsigned int heater)
{
	//TODO report the heater fault to the main board
}

void Platform::MessageF(MessageType type, const char *fmt, va_list vargs)
{
	String<FormatStringLength> formatString;
	if ((type & ErrorMessageFlag) != 0)
	{
		formatString.copy("Error: ");
		formatString.vcatf(fmt, vargs);
	}
	else if ((type & WarningMessageFlag) != 0)
	{
		formatString.copy("Warning: ");
		formatString.vcatf(fmt, vargs);
	}
	else
	{
		formatString.vprintf(fmt, vargs);
	}

	RawMessage((MessageType)(type & ~(ErrorMessageFlag | WarningMessageFlag)), formatString.c_str());
}

void Platform::MessageF(MessageType type, const char *fmt, ...)
{
	va_list vargs;
	va_start(vargs, fmt);
	MessageF(type, fmt, vargs);
	va_end(vargs);
}

void Platform::Message(MessageType type, const char *message)
{
	if ((type & (ErrorMessageFlag | WarningMessageFlag)) == 0)
	{
		RawMessage(type, message);
	}
	else
	{
		String<FormatStringLength> formatString;
		formatString.copy(((type & ErrorMessageFlag) != 0) ? "Error: " : "Warning: ");
		formatString.cat(message);
		RawMessage((MessageType)(type & ~(ErrorMessageFlag | WarningMessageFlag)), formatString.c_str());
	}
}

void Platform::LogError(ErrorCode e)
{
	errorCodeBits |= (uint32_t)e;
}

bool Platform::Debug(Module module)
{
	return false;
}

void Platform::SetDriversIdle() { }

float Platform::DriveStepsPerUnit(size_t drive) { return stepsPerMm[drive]; }

const float *Platform::GetDriveStepsPerUnit() { return stepsPerMm; }

void Platform::SetDriverStepTiming(size_t drive, const float timings[4])
{
	for (size_t i = 0; i < 4; ++i)
	{
		if (slowDriverStepTimingClocks[i] < timings[i])
		{
			slowDriverStepTimingClocks[i] = min<float>(timings[i], 50.0);
		}
	}
}

float Platform::GetPressureAdvance(size_t extruder) { return 0.4; }
//	void SetPressureAdvance(size_t extruder, float factor);
EndStopHit Platform::Stopped(size_t axisOrExtruder) { return EndStopHit::lowHit; }
bool Platform::EndStopInputState(size_t axis) { return false; }

void Platform::StepDriversLow()
{
	StepPio->OUTCLR.reg = allDriverBits;
}

void Platform::StepDriversHigh(uint32_t driverMap)
{
	StepPio->OUTSET.reg = driverMap;
}

//	uint32_t CalcDriverBitmap(size_t driver);

uint32_t Platform::GetDriversBitmap(size_t axisOrExtruder) 	// get the bitmap of driver step bits for this axis or extruder
{
	return driveDriverBits[axisOrExtruder];
}

//	unsigned int GetProhibitedExtruderMovements(unsigned int extrusions, unsigned int retractions);

void Platform::SetDirectionValue(size_t drive, bool dVal)
{
	if (drive < NumDrivers)
	{
		directions[drive] = dVal;
	}
}

bool Platform::GetDirectionValue(size_t driver)
{
	return (driver < NumDrivers) && directions[driver];
}

void Platform::SetDirection(size_t driver, bool direction)
{
	if (driver < NumDrivers)
	{
		const bool d = (direction) ? directions[driver] : !directions[driver];
		const bool isSlowDriver = IsBitSet(slowDriversBitmap, driver);
		if (isSlowDriver)
		{
			while (StepTimer::GetInterruptClocks() - DDA::lastStepLowTime < GetSlowDriverDirHoldClocks()) { }
		}
		digitalWrite(DirectionPins[driver], d);
		if (isSlowDriver)
		{
			DDA::lastDirChangeTime = StepTimer::GetInterruptClocks();
		}
	}
}

// The following don't do anything yet
void Platform::SetEnableValue(size_t driver, int8_t eVal)
{
	if (driver < NumDrivers)
	{
		enableValues[driver] = eVal;
	}
}

int8_t Platform::GetEnableValue(size_t driver)
{
	return (driver < NumDrivers) ? enableValues[driver] : 0;
}

EndStopHit Platform::GetZProbeResult()
{
	return EndStopHit::lowHit;
}

void Platform::EnableDrive(size_t driver)
{
	SmartDrivers::EnableDrive(driver, true);
}

void Platform::DisableDrive(size_t driver)
{
	SmartDrivers::EnableDrive(driver, false);
}

void Platform::DisableAllDrives()
{
	for (size_t driver = 0; driver < NumDrivers; ++driver)
	{
		SmartDrivers::EnableDrive(driver, false);
	}
}

//	void Platform::DisableDrive(size_t axisOrExtruder);
//	void Platform::DisableAllDrives();
//	void Platform::SetDriversIdle();

uint8_t Platform::ReadBoardId()
{
#ifdef SAMC21
	return 10;			//TODO temporary!
#else
	uint8_t rslt = 0;
	for (unsigned int i = 0; i < 4; ++i)
	{
		if (!digitalRead(BoardAddressPins[i]))
		{
			rslt |= 1 << i;
		}
	}
	return rslt;
#endif
}

// TMC driver temperatures
float Platform::GetTmcDriversTemperature()
{
	const uint16_t mask = (1u << MaxSmartDrivers) - 1;
	return ((temperatureShutdownDrivers & mask) != 0) ? 150.0
			: ((temperatureWarningDrivers & mask) != 0) ? 100.0
				: 0.0;
}

void Platform::Tick()
{
	//TODO
}

void Platform::StartFirmwareUpdate()
{
	doFirmwareUpdate = true;
}

// End
