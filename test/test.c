
#include "tinyunit.h"
#include "test-list.h"

int main(int argc, char **argv) {
  set_tests_categories("MQTT");
  run_tests();
  //run_test_mqtt_connect_tcp();
  //run_test_mqtt_singe_pub_single_single_sub_1000_msgs_qos_2_tls();
}