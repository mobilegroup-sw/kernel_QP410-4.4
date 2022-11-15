#ifndef __JMT_H__
#define __JMT_H__

/*
 *  Set chip definition here
 * */
#define JMT_SPI_CLOCK_SPEED         (16 * 1000 * 1000) 
#define JMT_VER_ADDR                0xff
#define JMT_HEARTBEAT_COUNT         8000
#define JMT_HIST_LEN                16
#define JMT_HISTOGRAM_TOO_WHITE     950
#define JMT_TOGGLE_RESET            0x01
#define JMT_INTENSITY_TOO_WHITE     1
#define JMT_INTENSITY_WHITE         60
#define JMT_INTENSITY_MEAN_WHITE    160
#define JMT_INTENSITY_NORMAL        180
#define JMT_INTENSITY_MEAN_BLACK    200
#define JMT_INTENSITY_BLACK         245
#define JMT_INTENSITY_TOO_BLACK     255

#define JMT_TOGGLE_RESET            0x01
#define JMT_SWITCH_CALIBRATE        0x01
#define JMT_SWITCH_TIMESTAMP        0x02
#define JMT_SWITCH_PARAMETER        0x04

#define JMT_REG_FRAME_DATA          0xD0
#define JMT_REG_SOFTWARE_RESET      0xD1
#define JMT_REG_MODE_SELECT         0xD2
#define JMT_REG_STATUS              0xD3
#define JMT_REG_POWER_CTRL          0xE4
#define JMT_REG_VER_ID_MINOR        0xFD
#define JMT_REG_VER_ID_MAJOR        0xFF

//#define JMT_IMAGE_BUFFER_SIZE       (256 * 128 * 128)
#define JMT_IMAGE_BUFFER_SIZE       (256 * 128 * 8)
#define JMT_FRAME_READY_LIMIT       500
#endif

