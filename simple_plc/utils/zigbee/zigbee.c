#include "zigbee.h"
#include <string.h>
#include "logger.h"

#define TIME_RESET_WHEN_NOT_RECEIVER_DATA 60000

/**
 * @file zigbee.c
 * @brief Zigbee communication implementation.
 * 
 * This module provides core functionality for the SynaptiX RS485/RF IO system.
 * 
 * @author SynaptiX Development Team
 * @version 1.0
 * @date 2026
 */

static const char *TAG = "Zigbee";

static uint8_t linkIns[9] = {0xFC, 0x06, 0x04, 0x44, 0x54, 0x4B, 0x52, 0x46, 0x81};          //---INS01
static uint8_t restartIns[9] = {0xFC, 0x06, 0x06, 0x44, 0x54, 0x4B, 0xAA, 0xBB, 0x50};       //---INS02
static uint8_t readIns[9] = {0xFC, 0x06, 0x0E, 0x44, 0x54, 0x4B, 0x52, 0x46, 0x8B};          //---INS05
static uint8_t writeIns[42];                                                                 //--INS06
static uint8_t queryEndDevicePosition[7] = {0xFC,0x04,0x0A,0x22,0x33,0x44,0xA3};
static uint8_t requestSignalIns[9] = {0xFC, 0x06, 0x0C, 0x44, 0x54, 0x4B, 0x52, 0x46, 0x89}; //--INS08
static uint8_t enterLowPowerIns[9] = {0xFC, 0x06, 0x12, 0x44, 0x54, 0x4B, 0x43, 0x4F, 0x89};
void  zigbeeGetWriteIns(ZigbeeMesh_t *this);
static void zigbee_send_connect_cmd(ZigbeeMesh_t *zigbee){
    zigbee->driver->write(linkIns, sizeof(linkIns));
}
static void zigbee_send_restart_cmd(ZigbeeMesh_t *zigbee){
    zigbee->driver->write(restartIns, sizeof(restartIns));
}
static void zigbee_send_read_cmd(ZigbeeMesh_t *zigbee){
    zigbee->driver->write(readIns, sizeof(readIns));
}
static void zigbee_send_write_cmd(ZigbeeMesh_t *zigbee){
    zigbeeGetWriteIns(zigbee);
    zigbee->driver->write(writeIns, sizeof(writeIns));
}
static void zigbee_send_request_signal_cmd(ZigbeeMesh_t *zigbee){
    zigbee->driver->write(requestSignalIns, sizeof(requestSignalIns));
}
static void zigbee_send_enter_lowpower_cmd(ZigbeeMesh_t *zigbee){
    zigbee->driver->write(enterLowPowerIns, sizeof(enterLowPowerIns));
}
static void zigbee_query_end_device_pos(ZigbeeMesh_t *zigbee){
    zigbee->driver->write(queryEndDevicePosition,7);
}

static uint8_t zigbeeGetXY(uint8_t *inputData, uint16_t inputLen)
{
    uint8_t result = 0;
    uint16_t i = 0;

    for (i = 0; i < inputLen - 1; i++)
    {
        result = result + inputData[i];
    }
    return result;
}
uint8_t   zigbeeIsInsBack(uint8_t *inputData, uint16_t  inputLen)
{
	uint8_t  result =0;
	uint8_t  result_count =0;
	uint8_t  result_XY =0;

	if(inputData[0] == 0xFA)  result_count++;
	if(inputLen == inputData[1] +4)   result_count++;

	result_XY = zigbeeGetXY(inputData, inputLen);
	if(inputData[inputLen-1] == result_XY)  result_count++;

	if(result_count == 3)   result =1;

	return   result;
}
void  zigbeeGetWriteIns(ZigbeeMesh_t *this)
{
	uint8_t   i=0;
	uint8_t  tempXY=0;
	// uint8_t writeHeaderLen=3;
	writeIns[0]= 0xFC;
	writeIns[1]= 0x27;
	writeIns[2]= 0x07;
	
	writeIns[writeHeaderLen +0]= this->param.pointType;
	writeIns[writeHeaderLen +1]= (this->param.PAN_ID & 0xFF00)>>8;
	writeIns[writeHeaderLen +2]= this->param.PAN_ID & 0x00FF;
	writeIns[writeHeaderLen +3]= this->param.Channel;
	writeIns[writeHeaderLen +4]= this->param.transferModel;
	writeIns[writeHeaderLen +5]= (this->param.userAddress & 0xFF00)>>8;
	writeIns[writeHeaderLen +6]= this->param.userAddress & 0x00FF;
	writeIns[writeHeaderLen +7]= (this->param.X7X8 & 0xFF00)>>8;
	writeIns[writeHeaderLen +8]= this->param.X7X8 & 0x00FF;
	writeIns[writeHeaderLen +9]= this->param.uartBraudRate;
	writeIns[writeHeaderLen +10]= this->param.uartDataBits;
	writeIns[writeHeaderLen +11]= this->param.uartStopBits;
	writeIns[writeHeaderLen +12]= this->param.uartParity;
	writeIns[writeHeaderLen +13]= (this->param.X13X14 & 0xFF00)>>8;
	writeIns[writeHeaderLen +14]= this->param.X13X14 & 0x00FF;
	writeIns[writeHeaderLen +15]= this->param.antennaSelect;
	
	writeIns[writeHeaderLen +16]= this->param.prePointType;
	writeIns[writeHeaderLen +17]= (this->param.prePAN_ID & 0xFF00)>>8;
	writeIns[writeHeaderLen +18]= this->param.prePAN_ID & 0x00FF;
	writeIns[writeHeaderLen +19]= this->param.preChannel;
	writeIns[writeHeaderLen +20]= this->param.preTransferModel;
	writeIns[writeHeaderLen +21]= (this->param.preUserAddress & 0xFF00)>>8;
	writeIns[writeHeaderLen +22]= this->param.preUserAddress & 0x00FF;
	writeIns[writeHeaderLen +23]= (this->param.X31X32 & 0xFF00)>>8;  //--AS ReadParameter's X31 X32
	writeIns[writeHeaderLen +24]= this->param.X31X32 & 0x00FF;       //--AS ReadParameter's X31 X32
	writeIns[writeHeaderLen +25]= this->param.preUartBraudRate;
	writeIns[writeHeaderLen +26]= this->param.preUartDataBits;
	writeIns[writeHeaderLen +27]= this->param.preUartStopBits;
	writeIns[writeHeaderLen +28]= this->param.preUartParity;
	writeIns[writeHeaderLen +29]= (this->param.X37X38 & 0xFF00)>>8;  //--AS ReadParameter's X37 X38
	writeIns[writeHeaderLen +30]= this->param.X37X38 & 0x00FF;       //--AS ReadParameter's X37 X38
	writeIns[writeHeaderLen +31]= this->param.preAntennaSelect;
	writeIns[writeHeaderLen +32]= 0x1;
	writeIns[writeHeaderLen +33]= this->param.isSecurity;
	
	for(i=0; i<4; i++)
	{
		writeIns[writeHeaderLen +34 +i]= this->param.securityCode[i];
	}
	
	tempXY = zigbeeGetXY(writeIns, writeInsLen);
	
	writeIns[writeInsLen-1]= tempXY;	
}

static uint8_t  zigbeeReadProcess(ZigbeeMesh_t *this, uint8_t  *inputData,  uint16_t  inputLen)
{
	uint8_t  result=ZIGBEE_RES_FAIL;

	uint8_t  tempXY=0;
	uint8_t  tempRight=0;
	uint8_t  i=0;
	
	tempXY = zigbeeGetXY(inputData, inputLen);
	
	if(inputData[0]==0xFA) { tempRight++; }
	if(inputData[1]==0x31) { tempRight++; }
	if(inputData[2]==0x0A) { tempRight++; }
	if(inputData[inputLen-1]==tempXY) { tempRight++; }
	
	if(tempRight==4)
	{
		//------
		this->param.pointType = inputData[readHeaderLen +0];
		this->param.PAN_ID = inputData[readHeaderLen +1]*256 + inputData[readHeaderLen +2];
		this->param.Channel = inputData[readHeaderLen +3];
		this->param.transferModel = inputData[readHeaderLen +4];
		this->param.userAddress = inputData[readHeaderLen +5]*256 + inputData[readHeaderLen +6];
		this->param.X7X8 = inputData[readHeaderLen +7]*256 + inputData[readHeaderLen +8];
		this->param.uartBraudRate = inputData[readHeaderLen +9];
		this->param.uartDataBits = inputData[readHeaderLen +10];
		this->param.uartStopBits = inputData[readHeaderLen +11];
		this->param.uartParity = inputData[readHeaderLen +12];
		this->param.X13X14 = inputData[readHeaderLen +13]*256 + inputData[readHeaderLen +14];
		this->param.antennaSelect = inputData[readHeaderLen +15];
		
		//-------
		for(i=0; i<8; i++)
		{
			this->param.macAddress[i] = inputData[readHeaderLen +16 +i];
		}
		
		//--------
		this->param.prePointType = inputData[readHeaderLen +24];
		this->param.prePAN_ID = inputData[readHeaderLen +25]*256 + inputData[readHeaderLen +26];
		this->param.preChannel = inputData[readHeaderLen +27];
		this->param.preTransferModel = inputData[readHeaderLen +28];
		this->param.preUserAddress = inputData[readHeaderLen +29]*256 + inputData[readHeaderLen +30];
		this->param.X31X32 = inputData[readHeaderLen +31]*256 + inputData[readHeaderLen +32];
		this->param.preUartBraudRate = inputData[readHeaderLen +33];
		this->param.preUartDataBits = inputData[readHeaderLen +34];
		this->param.preUartStopBits = inputData[readHeaderLen +35];
		this->param.preUartParity = inputData[readHeaderLen +36];
		this->param.X37X38 = inputData[readHeaderLen +37]*256 + inputData[readHeaderLen +38];
		this->param.preAntennaSelect = inputData[readHeaderLen +39];
		
		//-----------
		this->param.shortAddress = inputData[readHeaderLen +40]*256 + inputData[readHeaderLen +41];
		this->param.X42 = inputData[readHeaderLen +42];
		this->param.isSecurity = inputData[readHeaderLen +43];
		
		for(i=0; i<4; i++)
		{
			this->param.securityCode[i] = inputData[readHeaderLen +44 +i];
		}

		result = ZIGBEE_RES_SUCCESS;
	}

	return result;
}

static uint8_t zigbee_connect_process(ZigbeeMesh_t *this, uint8_t  *inputData,  uint16_t  inputLen)
{
    uint8_t  tempXY=0;
    uint8_t  tempRight=0;
    
    tempXY = zigbeeGetXY(inputData, inputLen);
    
    if(inputData[0]==0xFA) { tempRight++; }
    if(inputData[1]==0x06) { tempRight++; }
    if(inputData[2]==0x0A) { tempRight++; }
    if(inputData[inputLen-1]==tempXY) { tempRight++; }
    
    if(tempRight==4)
    {
        // this->module_type = inputData[connectHeaderLen +0];
        // this->fw_version = inputData[connectHeaderLen +1];
        this->fw_version[0] = ((inputData[7] << 8) | inputData[8]) / 10;
        
        return ZIGBEE_RES_SUCCESS;
    }
    return ZIGBEE_RES_FAIL;
}
static uint8_t zigbee_reset_process(ZigbeeMesh_t *this, uint8_t  *inputData,  uint16_t  inputLen)
{
    uint8_t  tempXY=0;
    uint8_t  tempRight=0;
    
    tempXY = zigbeeGetXY(inputData, inputLen);
    
    if(inputData[0]==0xFA) { tempRight++; }
    if(inputData[1]==0x06) { tempRight++; }
    if(inputData[2]==0x0A) { tempRight++; }
    if(inputData[inputLen-1]==tempXY) { tempRight++; }
    
    if(tempRight==4)
    {
        return ZIGBEE_RES_SUCCESS;
    }
    return ZIGBEE_RES_FAIL;
}
static uint8_t zigbee_write_process(ZigbeeMesh_t *this, uint8_t  *inputData,  uint16_t  inputLen)
{
    uint8_t  tempXY=0;
    uint8_t  tempRight=0;

    tempXY = zigbeeGetXY(inputData, inputLen);
    if(inputData[0]==0xFA) { tempRight++; }
    if(inputData[1]==0x01) { tempRight++; }
    if(inputData[2]==0x0A) { tempRight++; }
    if(inputData[inputLen-1]==tempXY) { tempRight++; }
    if(tempRight==4)
    {
        return ZIGBEE_RES_SUCCESS;
    }
    return ZIGBEE_RES_FAIL;
}
static uint8_t zigbee_request_signal_process(ZigbeeMesh_t *this, uint8_t  *inputData,  uint16_t  inputLen)
{
    uint8_t  tempXY=0;
    uint8_t  tempRight=0;

    tempXY = zigbeeGetXY(inputData, inputLen);
    if(inputData[0]==0xFA) { tempRight++; }
    if(inputData[1]==0x04) { tempRight++; }
    if(inputData[2]==0x0A) { tempRight++; }
    if(inputData[inputLen-1]==tempXY) { tempRight++; }
    if(tempRight==4)
    {
        return ZIGBEE_RES_SUCCESS;
    }
    return ZIGBEE_RES_FAIL;
}

static uint8_t zigbee_query_ed_process(ZigbeeMesh_t *this, uint8_t  *inputData,  uint16_t  inputLen){
    return ZIGBEE_RES_SUCCESS;
}

void zigbee_init(ZigbeeMesh_t *zigbee, ZigbeeDriver_t *driver)
{
    zigbee->driver = driver;
    memset(&zigbee->param, 0, sizeof(ZigbeeParameter_t));
    memset(&zigbee->event, 0, sizeof(ZigbeeEvent_t));
    memset(&zigbee->event_tmp, 0, sizeof(ZigbeeEvent_t));
    zigbee->time_reset = 0;
    splc_cqueue_init_static(&zigbee->event_queue,(void*)zigbee->event_list,sizeof(zigbee->event_list), sizeof(ZigbeeEvent_t));
    log_info(TAG, "Zigbee Init");
}
void zigbee_poll(ZigbeeMesh_t *zigbee, uint32_t timestamp)
{
    if(zigbee->event.event == ZB_EVENT_NONE)
    {
        if(zigbee->event_queue.count > 0)
        {
            splc_cqueue_receive(&zigbee->event_queue, &zigbee->event);
        }
        switch (zigbee->event.event)
        {
        case ZB_EVENT_NONE:
            /* code */
            if(zigbee->driver->available() > 0){
                zigbee->time_reset = 0;
            }
            zigbee->time_reset += timestamp;
            if(zigbee->time_reset >= TIME_RESET_WHEN_NOT_RECEIVER_DATA){
                log_info(TAG, "Zigbee Restart");
                zigbee_send_restart_cmd(zigbee);
                zigbee->time_reset = 0;
            }
            break;
        case ZB_EVENT_LINK_MODULE:
            /* code */
            zigbee_send_connect_cmd(zigbee);
            break;
        case ZB_EVENT_READ_MODULE:
            /* code */
            zigbee_send_read_cmd(zigbee);
            break;
        case ZB_EVENT_WRITE_MODULE:
            /* code */
            zigbee_send_write_cmd(zigbee);
            break;
        case ZB_EVENT_RESET_MODULE:
            /* code */
            zigbee_send_restart_cmd(zigbee);
            break;
        case ZB_EVENT_ENTER_LOWPOWER:
            /* code */
            zigbee_send_enter_lowpower_cmd(zigbee);
            break;
        case ZB_EVENT_GET_POSITION:
            /* code */
            zigbee_send_request_signal_cmd(zigbee);
            break;
        case ZB_EVENT_QUERY_POS:
            zigbee_query_end_device_pos(zigbee);
        default:
            break;
        }
    }
    if (zigbee->event.tick < zigbee->event.timeout && zigbee->event.event != ZB_EVENT_NONE)
    {
        zigbee->event.tick += timestamp;
    }
    if (zigbee->event.tick >= zigbee->event.timeout && zigbee->event.event != ZB_EVENT_NONE)
    {
        zigbee->buff_len = 0;
        zigbee->event.tick = 0;
        zigbee->buff_len = zigbee->driver->available();
        if (zigbee->buff_len > 0)
        {
            if (zigbee->buff_len > ZIGBEE_BUFFER_SIZE)
            {
                zigbee->buff_len = ZIGBEE_BUFFER_SIZE;
            }
            zigbee->driver->read(zigbee->buff, zigbee->buff_len);
            log_debug(TAG, "Zigbee Read : %u", zigbee->buff_len);
            log_print_hex(LOGGER_DEBUG, TAG, zigbee->buff, zigbee->buff_len);
        }
        if(zigbee->buff_len == 0 && zigbee->event.cb != NULL){
            zigbee->event.cb(zigbee,ZIGBEE_RES_TIMEOUT,zigbee->event.arg);
        }
        uint8_t res = ZIGBEE_RES_FAIL;
        switch (zigbee->event.event)
        {
        case ZB_EVENT_LINK_MODULE:
            res = zigbee_connect_process(zigbee, zigbee->buff, zigbee->buff_len);
            break;
        case ZB_EVENT_READ_MODULE:
            /* code */
            res = zigbeeReadProcess(zigbee, zigbee->buff, zigbee->buff_len);
            break;
        case ZB_EVENT_WRITE_MODULE:
            /* code */
            res = zigbee_write_process(zigbee, zigbee->buff, zigbee->buff_len);
            break;
        case ZB_EVENT_RESET_MODULE:
            /* code */
            res = zigbee_reset_process(zigbee, zigbee->buff, zigbee->buff_len);
            break;
        case ZB_EVENT_GET_POSITION:
            /* code */
            res = zigbee_request_signal_process(zigbee, zigbee->buff, zigbee->buff_len);
            break;
        case ZB_EVENT_QUERY_POS:
            res = zigbee_query_ed_process(zigbee,zigbee->buff,zigbee->buff_len);
            break;
        default:
            break;
        }
        if(res != ZIGBEE_RES_FAIL && zigbee->event.cb != NULL){
            zigbee->event.cb(zigbee,res,zigbee->event.arg);
        }
        zigbee->buff_len = 0;
        zigbee->event.tick = 0;
        zigbee->event.event = ZB_EVENT_NONE;
    }
}
void zigbee_connect(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg)
{
    zigbee->event_tmp.event = ZB_EVENT_LINK_MODULE;
    zigbee->event_tmp.timeout = 300;
    zigbee->event_tmp.tick = 0;
    zigbee->event_tmp.cb = cb;
    zigbee->event_tmp.arg = arg;
    splc_cqueue_send(&zigbee->event_queue, &zigbee->event_tmp);

}
void zigbee_reset_module(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg)
{

    zigbee->event_tmp.event = ZB_EVENT_RESET_MODULE;
    zigbee->event_tmp.timeout = 300;
    zigbee->event_tmp.tick = 0;
    zigbee->event_tmp.cb = cb;
    zigbee->event_tmp.arg = arg;
    splc_cqueue_send(&zigbee->event_queue, &zigbee->event_tmp);
}
void zigbee_read_module(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg)
{

    zigbee->event_tmp.event = ZB_EVENT_READ_MODULE;
    zigbee->event_tmp.timeout = 1000;
    zigbee->event_tmp.tick = 0;
    zigbee->event_tmp.cb = cb;
    zigbee->event_tmp.arg = arg;
    splc_cqueue_send(&zigbee->event_queue, &zigbee->event_tmp);
}
void zigbee_write_module(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg)
{
    zigbeeGetWriteIns(zigbee);
    log_info(TAG, "Zigbee Write : %u", sizeof(writeIns));
    log_info(TAG,"pointType: %u", zigbee->param.pointType);
    log_info(TAG,"PAN_ID: 0x%04X", zigbee->param.PAN_ID);
    log_info(TAG,"Channel: %u", zigbee->param.Channel);
    log_info(TAG,"transferModel: %u", zigbee->param.transferModel);
    log_info(TAG,"userAddress: 0x%04X", zigbee->param.userAddress);
    log_info(TAG,"uartBraudRate: %u", zigbee->param.uartBraudRate);
    log_info(TAG,"uartDataBits: %u", zigbee->param.uartDataBits);
    log_info(TAG,"uartStopBits: %u", zigbee->param.uartStopBits);
    log_info(TAG,"uartParity: %u", zigbee->param.uartParity);
    log_info(TAG,"antennaSelect: %u", zigbee->param.antennaSelect);
    log_info(TAG,"macAddress: ");
    log_print_hex(LOGGER_INFO, TAG, zigbee->param.macAddress, 8);
    log_info(TAG,"isSecurity: %u", zigbee->param.isSecurity);
    log_info(TAG,"securityCode: ");
    log_print_hex(LOGGER_INFO, TAG, zigbee->param.securityCode, 4);
    // zigbee->driver->write(writeIns, sizeof(writeIns));
    zigbee->event_tmp.event = ZB_EVENT_WRITE_MODULE;
    zigbee->event_tmp.timeout = 1000;
    zigbee->event_tmp.tick = 0;
    zigbee->event_tmp.cb = cb;
    zigbee->event_tmp.arg = arg;
    splc_cqueue_send(&zigbee->event_queue, &zigbee->event_tmp);
}

void zigbee_request_signal(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg)
{
    zigbee->event_tmp.event = ZB_EVENT_GET_POSITION;
    zigbee->event_tmp.timeout = 300;
    zigbee->event_tmp.tick = 0;
    zigbee->event_tmp.cb = cb;
    zigbee->event_tmp.arg = arg;
    splc_cqueue_send(&zigbee->event_queue, &zigbee->event_tmp);
}

void zigbee_query_ed_pos(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb, void *arg)
{
    if(zigbee->param.pointType != 1){
        log_error(TAG,"Node isn't Coordinator");
        return;
    }
    zigbee->event_tmp.event = ZB_EVENT_QUERY_POS;
    zigbee->event_tmp.timeout = 2000;
    zigbee->event_tmp.tick = 0;
    zigbee->event_tmp.cb = cb;
    zigbee->event_tmp.arg = arg;
    splc_cqueue_send(&zigbee->event_queue, &zigbee->event_tmp);
}
