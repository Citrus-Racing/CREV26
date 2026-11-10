// Sets the USB product name, so this board shows up in Linux as
// /dev/serial/by-id/usb-Teensyduino_SensorTest_LF_<serial number>-if00
// and find_boards.py shows LF is running the sensor test, not motor_node.
#include "usb_names.h"

#define PRODUCT_NAME      {'S','e','n','s','o','r','T','e','s','t','_','L','F'}
#define PRODUCT_NAME_LEN  13

struct usb_string_descriptor_struct usb_string_product_name = {
  2 + PRODUCT_NAME_LEN * 2,
  3,
  PRODUCT_NAME
};
