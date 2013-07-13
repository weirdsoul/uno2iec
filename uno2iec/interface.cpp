//
// Title        : RPI2UNO2IEC - interface implementation, arduino side.
// Author       : Lars Wadefalk
// Version      : 0.1
// Target MCU   : Arduino Uno AtMega328(H, 5V) at 16 MHz, 2KB SRAM, 32KB flash, 1KB EEPROM.
//
// CREDITS:
// --------
// The RPI2UNO2IEC application is inspired by Lars Pontoppidan's MMC2IEC project.
// It has been ported to C++.
// The MMC2IEC application is inspired from Jan Derogee's 1541-III project for
// PIC: http://jderogee.tripod.com/
// This code is a complete reimplementation though, which includes some new
// features and excludes others.
//
// DESCRIPTION:
// The interface connects all the loose ends in MMC2IEC.
//
// Commands from the IEC communication are interpreted, and the appropriate data
// from either Native, a D64 or T64 image is sent back.
//
// DISCLAIMER:
// The author is in no way responsible for any problems or damage caused by
// using this code. Use at your own risk.
//
// LICENSE:
// This code is distributed under the GNU Public License
// which can be found at http://www.gnu.org/licenses/gpl.txt
//

#include <string.h>
#include "global_defines.h"
#include "interface.h"

#ifdef CONSOLE_DEBUG
#include "log.h"
#endif


namespace {
// atn command buffer struct
IEC::ATNCmd cmd;
char serCmdIOBuf[80];
byte scrollBuffer[30];
} // unnamed namespace


Interface::Interface(IEC& iec)
	: m_iec(iec), m_pDisplay(0)
{
	reset();
} // ctor


void Interface::reset(void)
{
	m_openState = O_NOTHING;
	m_queuedError = ErrIntro;
	m_interfaceState = IS_NATIVE;
} // reset


void Interface::sendStatus(void)
{
	byte i;
	// Send error string
	const char* str = (const char*)error_table[m_queuedError];

	while((i = *(str++)))
		m_iec.send(i);

	// Send common ending string ,00,00
	for(i = 0; i < sizeof(errorEnding) - 1; ++i)
		m_iec.send(errorEnding[i]);

	// ...and last byte in string as with EOI marker.
	m_iec.sendEOI(errorEnding[i]);
} // sendStatus


// send basic line callback
void Interface::sendLineCallback(byte len, char* text)
{
	byte i;

	// Increment next line pointer
	// note: minus two here because the line number is included in the array already.
	m_basicPtr += len + 5 - 2;

	// Send that pointer
	m_iec.send(m_basicPtr bitand 0xFF);
	m_iec.send(m_basicPtr >> 8);

	// Send line number
//	m_iec.send(lineNo bitand 0xFF);
//	m_iec.send(lineNo >> 8);

	// Send line contents
	for(i = 0; i < len; i++)
		m_iec.send(text[i]);

	// Finish line
	m_iec.send(0);
} // sendLineCallback


void Interface::sendListing()
{
	noInterrupts();
	// Reset basic memory pointer:
	m_basicPtr = C64_BASIC_START;

	// Send load address
	m_iec.send(C64_BASIC_START bitand 0xff);
	m_iec.send((C64_BASIC_START >> 8) bitand 0xff);
	interrupts();
	// This will be slightly tricker: Need to specify the line sending protocol between raspberry and Arduino.
	// Call the listing function
	byte resp;
	do {
		Serial.write('L'); // initiate request.
		Serial.readBytes(serCmdIOBuf, 2);
		resp = serCmdIOBuf[0];
		if('L' == resp) { // PI will give us something else if we're at last line to send.
			// get the length as one byte: This is kind of specific: For listings we allow 256 bytes length. Period.
			byte len = serCmdIOBuf[1];
			byte actual = Serial.readBytes(serCmdIOBuf, len);
			if(len == actual) {
				// send the bytes directly to CBM!
				noInterrupts();
				sendLineCallback(len, serCmdIOBuf);
				interrupts();
			}
			else {
				resp = 'E'; // just to end the pain. We're out of sync or somthin'
				sprintf(serCmdIOBuf, "Expected: %d chars, got %d.", len, actual);
				Log(Error, FAC_IFACE, serCmdIOBuf);
			}
		}
		else {
			if('l' not_eq resp) {
				sprintf(serCmdIOBuf, "Ending at char: %d.", resp);
				Log(Error, FAC_IFACE, serCmdIOBuf);
				Serial.readBytes(serCmdIOBuf, sizeof(serCmdIOBuf));
				Log(Error, FAC_IFACE, serCmdIOBuf);
			}
		}
	} while('L' == resp); // keep looping for more lines as long as we got an 'L' indicating we haven't reached end.

	// End program
	noInterrupts();
	m_iec.send(0);
	m_iec.sendEOI(0);
	interrupts();
} // sendListing


void Interface::sendFile()
{
	// Send file bytes, such that the last one is sent with EOI.
	byte resp;
	Serial.write('S'); // ask for file size.
	byte len = Serial.readBytes(serCmdIOBuf, 3);
	// it is supposed to answer with S<highByte><LowByte>
	if(3 not_eq len or serCmdIOBuf[0] not_eq 'S')
		return;
	word written = 0;
	if(0 not_eq m_pDisplay)
		m_pDisplay->resetPercentage((serCmdIOBuf[1] << 8) bitor serCmdIOBuf[2]);

	do {
		Serial.write('R'); // ask for a byte
		len = Serial.readBytes(serCmdIOBuf, 2); // read the ack type ('B' or 'E')
		if(2 not_eq len) {
			Log(Error, FAC_IFACE, "Less than expected 2 bytes, stopping.");
			break;
		}
		resp = serCmdIOBuf[0];
		len = serCmdIOBuf[1];
		if('B' == resp or 'E' == resp) {
			byte actual = Serial.readBytes(serCmdIOBuf, len);
			if(actual not_eq len) {
				Log(Error, FAC_IFACE, "Less than expected bytes, stopping.");
				break;
			}
			bool success = true;
			// so we get some bytes, send them to CBM.
			for(byte i = 0; success and i < len; ++i) { // End if sending to CBM fails.
				noInterrupts();
				if(resp == 'E' and i == len - 1)
					success = m_iec.sendEOI(serCmdIOBuf[i]); // indicate end of file.
				else
					success = m_iec.send(serCmdIOBuf[i]);
				interrupts();
				++written;
				if(!(written % 32) and 0 not_eq m_pDisplay)
					m_pDisplay->showPercentage(written);
			}
		}
		else if('E' not_eq resp)
			Log(Error, FAC_IFACE, "Got unexpected command response char.");
	} while(resp == 'B'); // keep asking for more as long as we don't get the 'E' or something else (indicating out of sync).
	if(0 not_eq m_pDisplay)
		m_pDisplay->showPercentage(written);
} // sendFile


void Interface::saveFile()
{
	// Recieve bytes until a EOI is detected
	do {
		byte c = m_iec.receive();
		// indicate to PI host that we want to write a byte.
		Serial.write('W');
		// and then we send the byte itself.
		Serial.write(c);
	} while(!(m_iec.state() bitand IEC::eoiFlag) and !(m_iec.state() bitand IEC::errorFlag));
} // saveFile


void Interface::handler(void)
{
	//  m_iec.setDeviceNumber(8);

	IEC::ATNCheck retATN = IEC::ATN_IDLE;
	if(m_iec.checkRESET()) {
		Log(Information, FAC_IFACE, "GOT RESET, INITIAL STATE");
		reset();
	}
	else {
		noInterrupts();
		retATN = m_iec.checkATN(cmd);
		interrupts();
	}

	if(retATN == IEC::ATN_ERROR) {
#ifdef CONSOLE_DEBUG
		Log(Error, FAC_IFACE, "ATNCMD: IEC_ERROR!");
#endif
		return;
	}

	// Did anything happen from the host side?
	if(retATN not_eq IEC::ATN_IDLE) {
		// A command is recieved, make cmd string null terminated
		cmd.str[cmd.strlen] = '\0';
#ifdef CONSOLE_DEBUG
//		{
//			sprintf(serCmdIOBuf, "ATNCMD code:%d cmd: %s (len: %d) retATN: %d", cmd.code, cmd.str, cmd.strlen, retATN);
//			Log(Information, FAC_IFACE, serCmdIOBuf);
//		}
#endif

		// lower nibble is the channel.
		byte chan = cmd.code bitand 0x0F;

		// check upper nibble, the command itself.
		switch(cmd.code bitand 0xF0) {
			case IEC::ATN_CODE_OPEN:
				handleATNCmdCodeOpen(cmd);
			break;


			case IEC::ATN_CODE_DATA:  // data channel opened
				if(retATN == IEC::ATN_CMD_TALK)
					handleATNCmdCodeDataTalk(chan);
				else if(retATN == IEC::ATN_CMD_LISTEN)
					handleATNCmdCodeDataListen();
				break;

			case IEC::ATN_CODE_CLOSE:
				// handle close with PI.
				handleATNCmdClose();
				break;
		}

		//BUSY_LED_OFF();
	}
} // handler


void Interface::setMaxDisplay(Max7219 *pDisplay)
{
	m_pDisplay = pDisplay;
} // setMaxDisplay


void Interface::handleATNCmdCodeOpen(IEC::ATNCmd& cmd)
{
	sprintf(serCmdIOBuf, "O%u|%s\r", cmd.code bitand 0xF, cmd.str);
	// NOTE: PI side handles BOTH file open command AND the command channel command (from the cmd.code).
	Serial.print(serCmdIOBuf);
	// Note: the pi response handling can be done LATER! We're in quick business with the CBM here!
} // handleATNCmdCodeOpen


void Interface::handleATNCmdCodeDataTalk(byte chan)
{
	byte lengthOrResult;
	boolean wasSuccess = false;
	if(lengthOrResult = Serial.readBytes(serCmdIOBuf, 3)) {
		// process response into m_queuedError.
		// Response: ><code in binary><CR>
		if('>' == serCmdIOBuf[0] and 3 == lengthOrResult) {
			lengthOrResult = serCmdIOBuf[1];
			wasSuccess = true;
		}
		else
			Log(Error, FAC_IFACE, serCmdIOBuf);
	}
	if(CMD_CHANNEL == chan) {
		m_queuedError = wasSuccess ? lengthOrResult : ErrSerialComm;
		// Send status message
		sendStatus();
		// go back to OK state, we have dispatched the error to IEC host now.
		m_queuedError = ErrOK;
	}
	else {
		m_openState = wasSuccess ? lengthOrResult : O_NOTHING;

		switch(m_openState) {
		case O_INFO:
			// Reset and send SD card info
			reset();
			// TODO: interface with PI (file system media info).
			sendListing();
			break;

		case O_FILE_ERR:
			// TODO: interface with pi for error info.
			sendListing(/*&send_file_err*/);
			break;

		case O_NOTHING: /*or (0 == pff)*/
			// Say file not found
			m_iec.sendFNF();
			break;

		case O_FILE:
			// Send program file
			sendFile();
			break;

		case O_DIR:
			// Send listing
			sendListing(/*(PFUNC_SEND_LISTING)(pff->send_listing)*/);
			break;
		}
	}
//	Log(Information, FAC_IFACE, serCmdIOBuf);
} // handleATNCmdCodeDataTalk


void Interface::handleATNCmdCodeDataListen()
{
	// We are about to save stuff
	if(0 == 0 /*pff*/) {
		// file format functions unavailable, save dummy
		saveFile(/*&dummy_1*/);
		m_queuedError = ErrDriveNotReady;
	}
	else {
		// Check conditions before saving
		boolean writeSuccess = true;
		if(m_openState not_eq O_SAVE_REPLACE) {
			// Not a save with replace, if file exists its an error
			if(m_queuedError not_eq ErrFileNotFound) {
				m_queuedError = ErrFileExists;
				writeSuccess = false;
			}
		}

		if(writeSuccess) {
			// No overwrite problem, try to create a file to save in:
			//writeSuccess = ((PFUNC_UCHAR_CSTR)(pff->newfile))(oldCmdStr);
			if(!writeSuccess)
				// unable to save, just say write protect
				m_queuedError = ErrWriteProtectOn;
			else
				m_queuedError = ErrOK;

		}

		// TODO: saveFile to RP.
		if(writeSuccess)
			saveFile(/*(PFUNC_UCHAR_CHAR)(pff->putc)*/);
		else
			saveFile(/*&dummy_1*/);
	}
} // handleATNCmdCodeDataListen


void Interface::handleATNCmdClose()
{
	// handle close of file. PI will return the name of the last loaded file to us.
	Serial.print("C");
	Serial.readBytes(serCmdIOBuf, 2);
	byte resp = serCmdIOBuf[0];
	if('N' == resp) { // N indicates we have a name.
		// get the length of the name as one byte.
		byte len = serCmdIOBuf[1];
		byte actual = Serial.readBytes(serCmdIOBuf, len);
		if(len == actual) {
			serCmdIOBuf[len] = '\0';
			strcpy((char*)scrollBuffer, "   LOADED: ");
			strcat((char*)scrollBuffer, serCmdIOBuf);
			if(0 not_eq m_pDisplay)
				m_pDisplay->resetScrollText(scrollBuffer);

		}
		else {
			sprintf(serCmdIOBuf, "Expected: %d chars, got %d.", len, actual);
			Log(Error, FAC_IFACE, serCmdIOBuf);
		}
	}
} // handleATNCmdClose
