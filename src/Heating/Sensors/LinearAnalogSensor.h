/*
 * LinearAnalogSensor.h
 *
 *  Created on: 16 Apr 2019
 *      Author: David
 */

#ifndef SRC_HEATING_SENSORS_LINEARANALOGSENSOR_H_
#define SRC_HEATING_SENSORS_LINEARANALOGSENSOR_H_

#include "SensorWithPort.h"

class LinearAnalogSensor : public SensorWithPort
{
public:
	LinearAnalogSensor(unsigned int sensorNum);

	GCodeResult Configure(const CanMessageGenericParser& parser, const StringRef& reply) override;

	static constexpr const char *TypeName = "linearanalog";

	void Poll() override;

private:
	void CalcDerivedParameters();

	// Configurable parameters
	unsigned int thermistorInputChannel;
	float lowTemp, highTemp;
	bool filtered;

	// Derived parameters
	int adcFilterChannel;
	float linearIncreasePerCount;

	static constexpr float DefaultLowTemp = 0.0;
	static constexpr float DefaultHighTemp = 100.0;

	// ADC resolution
	static constexpr unsigned int AdcBits = 12;										// the ADCs in the SAM processors are 12-bit
	static constexpr int32_t UnfilteredAdcRange = 1 << AdcBits;						// The readings we pass in should be in range 0..(AdcRange - 1)
	static constexpr unsigned int AdcOversampleBits = 2;							// we use 2-bit oversampling
	static constexpr int32_t FilteredAdcRange = 1 << (AdcBits + AdcOversampleBits);	// The readings we pass in should be in range 0..(AdcRange - 1)
};

#endif /* SRC_HEATING_SENSORS_LINEARANALOGSENSOR_H_ */
