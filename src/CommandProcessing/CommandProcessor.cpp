/*
 * CommandProcessor.cpp
 *
 *  Created on: 26 Jul 2019
 *      Author: David
 */

#include "CommandProcessor.h"
#include "CAN/CanSlaveInterface.h"
#include "CanMessageBuffer.h"
#include "GCodes/GCodeResult.h"
#include "Heating/Heat.h"
#include "CanMessageGenericParser.h"

GCodeResult ProcessM950(const CanMessageGeneric& msg, const StringRef& reply)
{
	CanMessageGenericParser parser(msg, M950Params);
	uint16_t deviceNumber;
	if (parser.GetUintParam('F', deviceNumber))
	{
		//TODO configure fan
		reply.copy("Fan configuration not implemented");
		return GCodeResult::error;
	}
	if (parser.GetUintParam('H', deviceNumber))
	{
		//TODO configure servo
		reply.copy("heater configuration not implemented");
		return GCodeResult::error;
	}
	if (parser.GetUintParam('P', deviceNumber))
	{
		//TODO configure gpio
		reply.copy("GPIO configuration not implemented");
		return GCodeResult::error;
	}
	if (parser.GetUintParam('S', deviceNumber))
	{
		//TODO configure servo
		reply.copy("GPIO configuration not implemented");
		return GCodeResult::error;
	}
	reply.copy("Missing FPSH parameter");
	return GCodeResult::error;
}

void CommandProcessor::Spin()
{
	CanMessageBuffer *buf = CanSlaveInterface::GetCanCommand();
	if (buf != nullptr)
	{
		String<MaxCanReplyLength> reply;
		GCodeResult rslt;

		switch (buf->id.MsgType())
		{
		case CanMessageType::m307:
			rslt = Heat::ProcessM307(buf->msg.generic, reply.GetRef());
			break;

		case CanMessageType::m308:
			rslt = Heat::ProcessM308(buf->msg.generic, reply.GetRef());
			break;

		case CanMessageType::m950:
			rslt = ProcessM950(buf->msg.generic, reply.GetRef());
			break;

		case CanMessageType::m906:
		default:
			reply.printf("Unknown message type %04x", (unsigned int)buf->id.MsgType());
			rslt = GCodeResult::error;
			break;						// unrecognised message type
		}

		CanMessageBuffer::Free(buf);
		//TODO send the reply
		(void)rslt;
	}
}

// End