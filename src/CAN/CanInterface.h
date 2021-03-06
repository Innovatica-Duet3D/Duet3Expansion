/*
 * CanInterface.h
 *
 *  Created on: 17 Sep 2018
 *      Author: David
 */

#ifndef SRC_CAN_CANINTERFACE_H_
#define SRC_CAN_CANINTERFACE_H_

#include <RepRapFirmware.h>
#include <CanId.h>
#include <CanMessageFormats.h>
#include <GCodes/GCodeResult.h>

struct CanMessageMovement;
class CanMessageBuffer;

namespace CanInterface
{
	void Init(CanAddress defaultBoardAddress);
	void Shutdown();
	void Diagnostics(const StringRef& reply);

	CanAddress GetCanAddress();
	GCodeResult ChangeAddressAndDataRate(const CanMessageSetAddressAndNormalTiming& msg, const StringRef& reply);
	bool GetCanMove(CanMessageMovement& move);
	bool Send(CanMessageBuffer *buf);
	bool SendAsync(CanMessageBuffer *buf);
	bool SendAndFree(CanMessageBuffer *buf);
	CanMessageBuffer *GetCanCommand();

	void SendAnnounce(CanMessageBuffer *buf);

	void MoveStoppedByZProbe();
	void WakeAsyncSenderFromIsr();
}

#endif /* SRC_CAN_CANINTERFACE_H_ */
