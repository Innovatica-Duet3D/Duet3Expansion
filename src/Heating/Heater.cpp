/*
 * Heater.cpp
 *
 *  Created on: 24 Jul 2019
 *      Author: David
 */

#include "Heater.h"
#include "Platform.h"
#include "Heat.h"
#include "Sensors/TemperatureSensor.h"

Heater::Heater(unsigned int num)
	: heaterNumber(num), sensorNumber(-1), requestedTemperature(0.0),
	  maxTempExcursion(DefaultMaxTempExcursion), maxHeatingFaultTime(DefaultMaxHeatingFaultTime)
{
}

Heater::~Heater()
{
}

// Set the process model returning true if successful
GCodeResult Heater::SetModel(float gain, float tc, float td, float maxPwm, float voltage, bool usePid, bool inverted, const StringRef& reply)
{
	const float temperatureLimit = GetHighestTemperatureLimit();
	const bool rslt = model.SetParameters(gain, tc, td, maxPwm, temperatureLimit, voltage, usePid, inverted);
	if (rslt)
	{
		if (model.IsEnabled())
		{
			const GCodeResult rslt = UpdateModel(reply);
			if (rslt != GCodeResult::ok)
			{
				return rslt;
			}
			const float predictedMaxTemp = gain + NormalAmbientTemperature;
			const float noWarnTemp = (temperatureLimit - NormalAmbientTemperature) * 1.5 + 50.0;		// allow 50% extra power plus enough for an extra 50C
			if (predictedMaxTemp > noWarnTemp)
			{
				reply.printf("Heater %u appears to be over-powered. If left on at full power, its temperature is predicted to reach %dC", GetHeaterNumber(), (int)predictedMaxTemp);
				return GCodeResult::warning;
			}
		}
		else
		{
			ResetHeater();
		}
		return GCodeResult::ok;
	}

	reply.copy("bad model parameters");
	return GCodeResult::error;
}

GCodeResult Heater::SetFaultDetectionParameters(float pMaxTempExcursion, float pMaxFaultTime)
{
	maxTempExcursion = pMaxTempExcursion;
	maxHeatingFaultTime = pMaxFaultTime;
	return GCodeResult::ok;
}

GCodeResult Heater::SetHeaterMonitors(const CanMessageSetHeaterMonitors& msg, const StringRef& reply)
{
	for (size_t i = 0; i < min<size_t>(msg.numMonitors, MaxMonitorsPerHeater); ++i)
	{
		monitors[i].Set(msg.monitors[i].sensor, msg.monitors[i].limit, (HeaterMonitorAction)msg.monitors[i].action, (HeaterMonitorTrigger)msg.monitors[i].trigger);
	}
	return GCodeResult::ok;
}

GCodeResult Heater::SetOrReportModel(unsigned int heater, const CanMessageUpdateHeaterModel& msg, const StringRef& reply) noexcept
{
	const GCodeResult rslt = SetModel(msg.gain, msg.timeConstant, msg.deadTime, msg.maxPwm, msg.standardVoltage, msg.usePid, msg.inverted, reply);
	if (msg.pidParametersOverridden && (rslt == GCodeResult::ok || rslt == GCodeResult::warning))
	{
		SetRawPidParameters(msg.kP, msg.recipTi, msg.tD);
	}
	return rslt;
}

GCodeResult Heater::SetTemperature(const CanMessageSetHeaterTemperature& msg, const StringRef& reply)
{
	switch (msg.command)
	{
	case CanMessageSetHeaterTemperature::commandNone:
		requestedTemperature = msg.setPoint;
		return GCodeResult::ok;

	case CanMessageSetHeaterTemperature::commandOff:
		requestedTemperature = msg.setPoint;
		SwitchOff();
		return GCodeResult::ok;

	case CanMessageSetHeaterTemperature::commandOn:
		requestedTemperature = msg.setPoint;
		SwitchOn();
		return GCodeResult::ok;

	case CanMessageSetHeaterTemperature::commandResetFault:
		requestedTemperature = msg.setPoint;
		ResetFault();
		return GCodeResult::ok;

	case CanMessageSetHeaterTemperature::commandSuspend:
		Suspend(true);
		return GCodeResult::ok;

	case CanMessageSetHeaterTemperature::commandUnsuspend:
		requestedTemperature = msg.setPoint;
		Suspend(false);
		return GCodeResult::ok;

	default:
		break;
	}

	reply.printf("Unknown command %u to heater %u", msg.command, heaterNumber);
	return GCodeResult::ok;
}

float Heater::GetHighestTemperatureLimit() const noexcept
{
	float limit = BadErrorTemperature;
	for (const HeaterMonitor& prot : monitors)
	{
		if (prot.GetTrigger() == HeaterMonitorTrigger::TemperatureExceeded)
		{
			const float t = prot.GetTemperatureLimit();
			if (limit == BadErrorTemperature || t > limit)
			{
				limit = t;
			}
		}
	}
	return limit;
}

float Heater::GetLowestTemperatureLimit() const noexcept
{
	float limit = ABS_ZERO;
	for (const HeaterMonitor& prot : monitors)
	{
		if (prot.GetTrigger() == HeaterMonitorTrigger::TemperatureTooLow)
		{
			const float t = prot.GetTemperatureLimit();
			if (limit == ABS_ZERO || t < limit)
			{
				limit = t;
			}
		}
	}
	return limit;
}

void Heater::SetModelDefaults() noexcept
{
	model.SetParameters(DefaultHotEndHeaterGain, DefaultHotEndHeaterTimeConstant, DefaultHotEndHeaterDeadTime, 1.0, DefaultHotEndTemperatureLimit, 0.0, true, false);
}

// End
