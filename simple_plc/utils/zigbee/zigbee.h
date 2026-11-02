#ifndef ZIGBEE_H
#define ZIGBEE_H

#ifdef __cplusplus
/**
 * @file zigbee.h
 * @brief Zigbee communication interface.
 * 
 * This module provides core functionality for the SynaptiX RS485/RF IO system.
 * 
 * @author SynaptiX Development Team
 * @version 1.0
 * @date 2026
 */

extern "C"
{
#endif

#include <stdint.h>
#include "splc_cqueue.h"

#define    sendIns_link             1
#define    sendIns_reset            2
#define    sendIns_read             5
#define    sendIns_write            6
#define    sednIns_getEndPosition   7
#define    sednIns_getPointSignal   8
#define    sendIns_enterLowPowe     9
#define    sendData                 10
	 
//----	 
#define   readHeaderLen    4
#define   writeHeaderLen   3
#define   writeInsLen      42
	 
#define   Coordinator   1
#define   Router        2
#define   EndDevice     3

#define   parencyTransfer          1
#define   parencyTransferUserAdd   2
#define   parencyTransferShortAdd  3
#define   parencyTransferMacAdd    4
#define   transferN_N              5
	 
#define   braudRate1200            1
#define   braudRate2400            2
#define   braudRate4800            3
#define   braudRate9600            4
#define   braudRate19200           5
#define   braudRate38400           6
#define   braudRate57600           7
#define   braudRate115200          8

#define   parityNon                1
#define   parityEven               2
#define   parityOdd                3          
	 
//-----
#define   INS01        1
#define   INS02        2
#define   INS03        3
#define   INS04        4
#define   INS05        5
#define   INS06        6
#define   INS07        7
#define   INS08        8
#define   INS09        9

#define ZIGBEE_BUFFER_SIZE 256*4
#define ZIGBEE_RES_SUCCESS 0x01
#define ZIGBEE_RES_FAIL 0x00
#define ZIGBEE_RES_TIMEOUT 0xFF

    typedef struct ZigbeeParameters
    {
        uint8_t pointType;     // X0
        uint16_t PAN_ID;       // X1 X2
        uint8_t Channel;       // X3
        uint8_t transferModel; // X4
        uint16_t userAddress;  // X5 X6
        uint16_t X7X8;         // X7 X8
        uint8_t uartBraudRate; // X9
        uint8_t uartDataBits;  // X10
        uint8_t uartStopBits;  // X11
        uint8_t uartParity;    // X12
        uint16_t X13X14;       // X13 X14
        uint8_t antennaSelect; // X15
        uint8_t macAddress[8]; // X16 - X23

        uint8_t prePointType;     // X24
        uint16_t prePAN_ID;       // X25 X26
        uint8_t preChannel;       // X27
        uint8_t preTransferModel; // X28
        uint16_t preUserAddress;  // X29 X30
        uint16_t X31X32;          // X31 X32
        uint8_t preUartBraudRate; // X33
        uint8_t preUartDataBits;  // X34
        uint8_t preUartStopBits;  // X35
        uint8_t preUartParity;    // X36
        uint16_t X37X38;          // X37 X38
        uint8_t preAntennaSelect; // X39

        uint16_t shortAddress;   // X40 X41
        uint8_t X42;             // X42
        uint8_t isSecurity;      // X43
        uint8_t securityCode[4]; // X44 - X47
    } ZigbeeParameter_t;

    typedef struct ZigbeeMesh ZigbeeMesh_t;

    typedef void (*ZB_Callback_Func)(ZigbeeMesh_t *Zigbee,uint8_t isSuccress,void *arg);
    typedef struct ZigbeeDriver
    {
        /* data */
        void (*write)(uint8_t *buff, uint32_t len);
        void (*read)(uint8_t *buff, uint32_t len);
        uint32_t (*available)();
    } ZigbeeDriver_t;

    typedef enum ZigbeeEventType
    {
        ZB_EVENT_NONE = 0,
        ZB_EVENT_LINK_MODULE,
        ZB_EVENT_READ_MODULE,
        ZB_EVENT_WRITE_MODULE,
        ZB_EVENT_RESET_MODULE,
        ZB_EVENT_ENTER_LOWPOWER,
        ZB_EVENT_GET_POSITION,
        ZB_EVENT_QUERY_POS
    } ZigbeeEventType_t;

    typedef struct ZigbeeEvent
    {
        /* data */
        uint32_t timeout;
        uint32_t tick;
        ZigbeeEventType_t event;
        void *arg;
        ZB_Callback_Func cb;
    } ZigbeeEvent_t;

    struct ZigbeeMesh
    {
        /* data */
        ZigbeeParameter_t param;
        ZigbeeDriver_t *driver;
        ZigbeeEvent_t event;
        ZigbeeEvent_t event_tmp;
        splc_cqueue_t event_queue;
        ZigbeeEvent_t event_list[10];
        uint8_t module_type;
        uint8_t fw_version[2];
        uint8_t buff[ZIGBEE_BUFFER_SIZE];
        uint32_t buff_len;
        uint32_t time_reset;
    };

    void zigbee_init(ZigbeeMesh_t *zigbee, ZigbeeDriver_t *driver);
    void zigbee_poll(ZigbeeMesh_t *zigbee, uint32_t timestamp);
    void zigbee_connect(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg);
    void zigbee_reset_module(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg);
    void zigbee_read_module(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg);
    void zigbee_write_module(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg);
    void zigbee_request_signal(ZigbeeMesh_t *zigbee, ZB_Callback_Func cb,void *arg);
    void zigbee_query_ed_pos(ZigbeeMesh_t *zigbee,ZB_Callback_Func cb,void *arg);
#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_H