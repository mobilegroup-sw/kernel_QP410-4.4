#ifndef _LINUX_SPI_JMT301_H_
#define _LINUX_SPI_JMT301_H_

struct jmt101_nav_settings {
   u8 finger_down_min;
   u8 finger_down_mid;
   u8 finger_down_max;
   u8 finger_detect_thr;
   u8 finger_lost_thr;
   u8 dx_thr;
   u8 dy_thr;
   u8 nav_cntr;
   u8 adc_offset;
   u8 adc_gain;
   u8 pixel_setup;
   u8 off_x_axis_thr;
   u8 off_y_axis_thr;
   u8 update;
   u8 enabled;
};

struct jmt101_platform_data {
   u32 irq_gpio;
   u32 reset_gpio;
   struct jmt101_nav_settings nav;
};

#endif
