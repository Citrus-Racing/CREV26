// Sets the USB product name, so the board shows up in Linux as
// /dev/serial/by-id/usb-Teensyduino_EncoderTest_<serial number>-if00
// and find_boards.py shows that corner is running the encoder test, not motor_node.
#include "usb_names.h"

#define PRODUCT_NAME      {'E','n','c','o','d','e','r','T','e','s','t'}
#define PRODUCT_NAME_LEN  11

struct usb_string_descriptor_struct usb_string_product_name = {
  2 + PRODUCT_NAME_LEN * 2,
  3,
  PRODUCT_NAME
};
