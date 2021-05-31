#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <assert.h>
#include <pthread.h>

#include <zmq.h>
#include <libusb-1.0/libusb.h>

#include <capnp/serialize.h>
#include "cereal/gen/cpp/log.capnp.h"
#include "cereal/gen/cpp/car.capnp.h"

#include "common/messaging.h"
#include "common/params.h"
#include "common/swaglog.h"
#include "common/timing.h"

#include <map>
#include <algorithm>

// double the FIFO size
#define RECV_SIZE (0x1000)
#define TIMEOUT 0

namespace {


volatile sig_atomic_t do_exit = 0;

libusb_context *ctx = NULL;
libusb_device_handle *dev_handle;
pthread_mutex_t usb_lock;

bool spoofing_started = false;
bool fake_send = false;
bool loopback_can = false;
cereal::HealthData::HwType hw_type = cereal::HealthData::HwType::UNKNOWN;
bool is_pigeon = false;
const uint32_t NO_IGNITION_CNT_MAX = 2 * 60 * 60 * 24 * 3;  // turn off charge after 3 days
uint32_t no_ignition_cnt = 0;
bool connected_once = false;
uint8_t ignition_last = 0;

pthread_t safety_setter_thread_handle = -1;
pthread_t pigeon_thread_handle = -1;
bool pigeon_needs_init;

int big_recv;
uint32_t big_data[RECV_SIZE*4];
std::map<uint32_t, uint64_t> message_index;
bool index_initialized = false;

void pigeon_init();
void *pigeon_thread(void *crap);

void *safety_setter_thread(void *s) {
  char *value_vin;
  size_t value_vin_sz = 0;

  // switch to no_output when CarVin param is read
  //while (1) {
  //  if (do_exit) return NULL;
  //  const int result = read_db_value(NULL, "CarVin", &value_vin, &value_vin_sz);
  //  if (value_vin_sz > 0) {
  //    // sanity check VIN format
  //    assert(value_vin_sz == 17);
  //    break;
  //  }
  //  usleep(100*1000);
  //}
  //LOGW("got CarVin %s", value_vin);

  //pthread_mutex_lock(&usb_lock);

  // VIN query done, stop listening to OBDII
  //libusb_control_transfer(dev_handle, 0x40, 0xdc, (uint16_t)(cereal::CarParams::SafetyModel::HONDA_BOSCH), 0, NULL, 0, TIMEOUT);

  //pthread_mutex_unlock(&usb_lock);

  char *value;
  size_t value_sz = 0;

  LOGW("waiting for params to set safety model");
  while (1) {
    if (do_exit) return NULL;

    const int result = read_db_value(NULL, "CarParams", &value, &value_sz);
    if (value_sz > 0) break;
    usleep(100*1000);
  }

  LOGW("got %d bytes CarParams", value_sz);

  // format for board, make copy due to alignment issues, will be freed on out of scope
  auto amsg = kj::heapArray<capnp::word>((value_sz / sizeof(capnp::word)) + 1);
  memcpy(amsg.begin(), value, value_sz);
  free(value);

  capnp::FlatArrayMessageReader cmsg(amsg);
  cereal::CarParams::Reader car_params = cmsg.getRoot<cereal::CarParams>();

  auto canIDs = car_params.getCanIds();
  for (int i=0; i<canIDs.size(); i++) {
    for (int j=0; j<canIDs[i].size(); j++) {
      message_index[canIDs[i][j]] = 0;
      LOGW("message id %d added", canIDs[i][j]);
    }
  }
  index_initialized = true;

  auto safety_model = car_params.getSafetyModel();
  auto safety_param = car_params.getSafetyParam();
  LOGW("setting safety model: %d with param %d", safety_model, safety_param);

  pthread_mutex_lock(&usb_lock);

  // set in the mutex to avoid race
  safety_setter_thread_handle = -1;

  // set if long_control is allowed by openpilot. Hardcoded to True for now
  libusb_control_transfer(dev_handle, 0x40, 0xdf, 1, 0, NULL, 0, TIMEOUT);

  libusb_control_transfer(dev_handle, 0x40, 0xdc, (uint16_t)safety_model, safety_param, NULL, 0, TIMEOUT);

  pthread_mutex_unlock(&usb_lock);

  return NULL;
}

// must be called before threads or with mutex
bool usb_connect() {
  int err;
  unsigned char hw_query[1] = {0};
  unsigned char serial_buf[16];
  const char *serial;
  int serial_sz = 0;
  ignition_last = 0;

  dev_handle = libusb_open_device_with_vid_pid(ctx, 0xbbaa, 0xddcc);
  if (dev_handle == NULL) { goto fail; }

  err = libusb_set_configuration(dev_handle, 1);
  if (err != 0) { goto fail; }

  err = libusb_claim_interface(dev_handle, 0);
  if (err != 0) { goto fail; }

  if (loopback_can) {
    libusb_control_transfer(dev_handle, 0xc0, 0xe5, 1, 0, NULL, 0, TIMEOUT);
  }

  // power off ESP
  libusb_control_transfer(dev_handle, 0xc0, 0xd9, 0, 0, NULL, 0, TIMEOUT);

  // get panda serial
  err = libusb_control_transfer(dev_handle, 0xc0, 0xd0, 0, 0, serial_buf, 16, TIMEOUT);

  /*if (err > 0) {
    serial = (const char *)serial_buf;
    serial_sz = strnlen(serial, err);
    write_db_value(NULL, "PandaDongleId", serial, serial_sz);
    LOGW("panda serial: %.*s\n", serial_sz, serial);
  }*/

  // power on charging, only the first time. Panda can also change mode and it causes a brief disconneciton
//#ifndef __x86_64__
  if (!connected_once) {
    libusb_control_transfer(dev_handle, 0xc0, 0xe6, (uint16_t)(cereal::HealthData::UsbPowerMode::CDP), 0, NULL, 0, TIMEOUT);
  }
//#endif
  connected_once = true;

  libusb_control_transfer(dev_handle, 0xc0, 0xc1, 0, 0, hw_query, 1, TIMEOUT);

  hw_type = (cereal::HealthData::HwType)(hw_query[0]);
  is_pigeon = (hw_type == cereal::HealthData::HwType::GREY_PANDA) ||
              (hw_type == cereal::HealthData::HwType::BLACK_PANDA); 
  if (is_pigeon) {
    LOGW("panda with gps detected");
    pigeon_needs_init = true;
    if (pigeon_thread_handle == -1) {
      err = pthread_create(&pigeon_thread_handle, NULL, pigeon_thread, NULL);
      assert(err == 0);
    }
  }

  return true;
fail:
  return false;
}


void usb_retry_connect() {
  LOGW("attempting to connect");
  while (!usb_connect()) { usleep(100*1000); }
  LOGW("connected to board");
}

void handle_usb_issue(int err, const char func[]) {
  LOGE_100("usb error %d \"%s\" in %s", err, libusb_strerror((enum libusb_error)err), func);
  if (err == -4) {
    LOGE("lost connection");
    usb_retry_connect();
  }
  // TODO: check other errors, is simply retrying okay?
}

uint64_t read_u64_be(const uint8_t* v) {
  return (((uint64_t)v[0] << 56)
          | ((uint64_t)v[1] << 48)
          | ((uint64_t)v[2] << 40)
          | ((uint64_t)v[3] << 32)
          | ((uint64_t)v[4] << 24)
          | ((uint64_t)v[5] << 16)
          | ((uint64_t)v[6] << 8)
          | (uint64_t)v[7]);
}

bool can_recv(void *s, bool force_send) {
  int err;
  uint32_t data[RECV_SIZE/4];
  int recv, big_index;
  uint32_t f1, f2, address;
  bool frame_sent;
  uint64_t cur_time;
  frame_sent = false;

  // do recv
  pthread_mutex_lock(&usb_lock);

  do {
    err = libusb_bulk_transfer(dev_handle, 0x81, (uint8_t*)data, RECV_SIZE, &recv, TIMEOUT);
    if (err != 0) { handle_usb_issue(err, __func__); }
    if (err == -8) { LOGW("overflow got 0x%x", recv); };

    // timeout is okay to exit, recv still happened
    if (err == -7) { break; }
  } while(err != 0);

  pthread_mutex_unlock(&usb_lock);

  // return if both buffers are empty
  /*if ((big_recv <= 0) && (recv <= 0)) {
  return true;~
  }*/
  if (recv <= 0) {
    return false;
  }

  // TODO: Split bus 0 and 1 into separate packets synced to 330 for bus 0 and 586 for bus 1
  big_index = big_recv/0x10;
  force_send = false;
  int j = 0;
  for (int i = 0; i<(recv/0x10); i++) {
    auto message = message_index.find(data[i*4] >> 21);
    if ((message != message_index.end()) | (index_initialized == false)) {
      if (data[i*4] >> 21 == 330) force_send = true;
      big_data[(big_index + j)*4] = data[i*4];
      big_data[(big_index + j)*4+1] = data[i*4+1];
      big_data[(big_index + j)*4+2] = data[i*4+2];
      big_data[(big_index + j)*4+3] = data[i*4+3];
      big_recv += 0x10;    
      j++;
    }
  }
  if (force_send) {
    frame_sent = true;

    capnp::MallocMessageBuilder msg;
    cereal::Event::Builder event = msg.initRoot<cereal::Event>();
    event.setLogMonoTime(nanos_since_boot());

    auto can_data = event.initCan(big_recv/0x10);

    // populate message
    for (int i = 0; i<(big_recv/0x10); i++) {
      if (big_data[i*4] & 4) {
        // extended
        can_data[i].setAddress(big_data[i*4] >> 3);
        //LOGW("got extended: %x\n", big_data[i*4] >> 3);
      } else {
        // normal
        can_data[i].setAddress(big_data[i*4] >> 21);
      }
      can_data[i].setBusTime(big_data[i*4+1] >> 16);
      int len = big_data[i*4+1]&0xF;
      can_data[i].setDat(kj::arrayPtr((uint8_t*)&big_data[i*4+2], len));
      can_data[i].setSrc((big_data[i*4+1] >> 4) & 0xff);
    }

    // send to can
    auto words = capnp::messageToFlatArray(msg);
    auto bytes = words.asBytes();
    zmq_send(s, bytes.begin(), bytes.size(), 0);
    big_recv = 0;
  }

  return frame_sent;
}

void can_health(void *s) {
  int cnt;
  int err;

  // copied from panda/board/main.c
  struct __attribute__((packed)) health {
    uint32_t voltage;
    uint32_t current;
    uint32_t can_send_errs;
    uint32_t can_fwd_errs;
    uint32_t gmlan_send_errs;
    uint8_t started;
    uint8_t controls_allowed;
    uint8_t gas_interceptor_detected;
    uint8_t car_harness_status;
    uint8_t usb_power_mode;
  } health;

  // recv from board
  pthread_mutex_lock(&usb_lock);

  do {
    cnt = libusb_control_transfer(dev_handle, 0xc0, 0xd2, 0, 0, (unsigned char*)&health, sizeof(health), TIMEOUT);
    if (cnt != sizeof(health)) {
      handle_usb_issue(cnt, __func__);
    }
  } while(cnt != sizeof(health));

  pthread_mutex_unlock(&usb_lock);

  if (health.started == 0) {
    no_ignition_cnt += 1;
  } else {
    no_ignition_cnt = 0;
  }

//#ifndef __x86_64__
//  if ((no_ignition_cnt > NO_IGNITION_CNT_MAX) && (health.usb_power_mode == (uint8_t)(cereal::HealthData::UsbPowerMode::CDP))) {
//    LOGW("TURN OFF CHARGING!\n");
//    pthread_mutex_lock(&usb_lock);
//    libusb_control_transfer(dev_handle, 0xc0, 0xe6, (uint16_t)(cereal::HealthData::UsbPowerMode::CLIENT), 0, NULL, 0, TIMEOUT);
//    pthread_mutex_unlock(&usb_lock);
//  }
//#endif

  // clear VIN, CarParams, and set new safety on car start
  if ((health.started != 0) && (ignition_last == 0)) {

    int result = delete_db_value(NULL, "CarVin");
    assert((result == 0) || (result == ERR_NO_VALUE));
    result = delete_db_value(NULL, "CarParams");
    assert((result == 0) || (result == ERR_NO_VALUE));

    // diagnostic only is the default, needed for VIN query
    //pthread_mutex_lock(&usb_lock);
    //libusb_control_transfer(dev_handle, 0x40, 0xdc, (uint16_t)(cereal::CarParams::SafetyModel::ELM327), 0, NULL, 0, TIMEOUT);
    //pthread_mutex_unlock(&usb_lock);

    if (safety_setter_thread_handle == -1) {
      err = pthread_create(&safety_setter_thread_handle, NULL, safety_setter_thread, NULL);
      assert(err == 0);
    }
  }

  ignition_last = health.started;

  // create message
  capnp::MallocMessageBuilder msg;
  cereal::Event::Builder event = msg.initRoot<cereal::Event>();
  event.setLogMonoTime(nanos_since_boot());
  auto healthData = event.initHealth();

  // set fields
  healthData.setVoltage(health.voltage);
  healthData.setCurrent(health.current);
  if (spoofing_started) {
    healthData.setStarted(1);
  } else {
    healthData.setStarted(health.started);
  }
  healthData.setControlsAllowed(health.controls_allowed);
  healthData.setGasInterceptorDetected(health.gas_interceptor_detected);
  healthData.setHasGps(is_pigeon);
  healthData.setCanSendErrs(health.can_send_errs);
  healthData.setCanFwdErrs(health.can_fwd_errs);
  healthData.setGmlanSendErrs(health.gmlan_send_errs);
  healthData.setHwType(hw_type);
  healthData.setUsbPowerMode(cereal::HealthData::UsbPowerMode(health.usb_power_mode));

  // send to health
  auto words = capnp::messageToFlatArray(msg);
  auto bytes = words.asBytes();
  zmq_send(s, bytes.begin(), bytes.size(), 0);

  pthread_mutex_lock(&usb_lock);

  // send heartbeat back to panda
  libusb_control_transfer(dev_handle, 0x40, 0xf3, 1, 0, NULL, 0, TIMEOUT);

  pthread_mutex_unlock(&usb_lock);
}


void can_send(void *s) {
  int err;

  // recv from sendcan
  zmq_msg_t msg;
  zmq_msg_init(&msg);
  err = zmq_msg_recv(&msg, s, 0);
  assert(err >= 0);

  // format for board, make copy due to alignment issues, will be freed on out of scope
  auto amsg = kj::heapArray<capnp::word>((zmq_msg_size(&msg) / sizeof(capnp::word)) + 1);
  memcpy(amsg.begin(), zmq_msg_data(&msg), zmq_msg_size(&msg));

  capnp::FlatArrayMessageReader cmsg(amsg);
  cereal::Event::Reader event = cmsg.getRoot<cereal::Event>();
  //if (nanos_since_boot() - event.getLogMonoTime() > 1e9) {
  //  //Older than 1 second. Dont send.
  //  zmq_msg_close(&msg);
  //  return;
  //}
  int msg_count = event.getCan().size();

  uint32_t *send = (uint32_t*)malloc(msg_count*0x10);
  memset(send, 0, msg_count*0x10);

  for (int i = 0; i < msg_count; i++) {
    auto cmsg = event.getSendcan()[i];
    if (cmsg.getAddress() >= 0x800) {
      // extended
      send[i*4] = (cmsg.getAddress() << 3) | 5;
    } else {
      // normal
      send[i*4] = (cmsg.getAddress() << 21) | 1;
    }
    assert(cmsg.getDat().size() <= 8);
    send[i*4+1] = cmsg.getDat().size() | (cmsg.getSrc() << 4);
    memcpy(&send[i*4+2], cmsg.getDat().begin(), cmsg.getDat().size());
  }

  // release msg
  zmq_msg_close(&msg);

  // send to board
  int sent;
  pthread_mutex_lock(&usb_lock);

  if (!fake_send) {
    do {
      err = libusb_bulk_transfer(dev_handle, 3, (uint8_t*)send, msg_count*0x10, &sent, TIMEOUT);
      if (err != 0 || msg_count*0x10 != sent) { handle_usb_issue(err, __func__); }
    } while(err != 0);
  }

  pthread_mutex_unlock(&usb_lock);

  // done
  free(send);
}

// **** threads ****

void *can_send_thread(void *crap) {
  LOGW("start send thread");

  // sendcan = 8017
  void *context = zmq_ctx_new();
  void *subscriber = sub_sock(context, "tcp://127.0.0.1:8017");

  // drain sendcan to delete any stale messages from previous runs
  zmq_msg_t msg;
  zmq_msg_init(&msg);
  int err = 0;
  while(err >= 0) {
    err = zmq_msg_recv(&msg, subscriber, ZMQ_DONTWAIT);
  }

  // run as fast as messages come in
  while (!do_exit) {
    can_send(subscriber);
  }
  return NULL;
}

void *can_recv_thread(void *crap) {
  LOGW("start recv thread");

  // can = 8006
  void *context = zmq_ctx_new();
  void *publisher = zmq_socket(context, ZMQ_PUB);
  zmq_bind(publisher, "tcp://*:8006");

  bool frame_sent, skip_once, force_send;
  uint64_t wake_time, cur_time, last_long_sleep;
  int recv_state = 0;
  int long_sleep_us = 3333;
  int panda_loops = 2;
  int short_sleep_us =  10000 - (panda_loops * long_sleep_us);
  force_send = true;
  last_long_sleep = 1e-3 * nanos_since_boot();
  wake_time = last_long_sleep;

  while (!do_exit) {

    frame_sent = can_recv(publisher, force_send);
    if (frame_sent) recv_state = 0;

    // drain the Panda with a sleep, then MAYBE once more if it is ahead of schedule
    if (recv_state++ < panda_loops) {
      last_long_sleep = 1e-3 * nanos_since_boot();
      wake_time += long_sleep_us;
      force_send = false;
      if (last_long_sleep < wake_time) {
        usleep(wake_time - last_long_sleep);
      }
      else {
        if ((last_long_sleep - wake_time) > 5e5) {
          // probably a new drive
          wake_time = last_long_sleep;
        }
        else {
          if (recv_state < 2) {
            wake_time += long_sleep_us;
            recv_state++;
            if (last_long_sleep < wake_time) {
              usleep(wake_time - last_long_sleep);
            }
            //else {
            //  LOGW("   boardd lagged, skip sleep! %d\n", recv_state);
            //}
          }
        }
      }
    }
    else {
      force_send = true;
      recv_state = 0;
      wake_time += short_sleep_us;
      cur_time = 1e-3 * nanos_since_boot();
      if (wake_time > cur_time) {
        usleep(wake_time - cur_time);
      }
    }
  }
  return NULL;
}

void *can_health_thread(void *crap) {
  LOGW("start health thread");
  // health = 8011
  void *context = zmq_ctx_new();
  void *publisher = zmq_socket(context, ZMQ_PUB);
  zmq_bind(publisher, "tcp://*:8011");

  // run at 2hz
  while (!do_exit) {
    can_health(publisher);
    usleep(500*1000);
  }
  return NULL;
}

#define pigeon_send(x) _pigeon_send(x, sizeof(x)-1)

void hexdump(unsigned char *d, int l) {
  for (int i = 0; i < l; i++) {
    if (i!=0 && i%0x10 == 0) printf("\n");
    printf("%2.2X ", d[i]);
  }
  printf("\n");
}

void _pigeon_send(const char *dat, int len) {
  int sent;
  unsigned char a[0x20];
  int err;
  a[0] = 1;
  for (int i=0; i<len; i+=0x20) {
    int ll = std::min(0x20, len-i);
    memcpy(&a[1], &dat[i], ll);
    pthread_mutex_lock(&usb_lock);
    err = libusb_bulk_transfer(dev_handle, 2, a, ll+1, &sent, TIMEOUT);
    if (err < 0) { handle_usb_issue(err, __func__); }
    /*assert(err == 0);
    assert(sent == ll+1);*/
    //hexdump(a, ll+1);
    pthread_mutex_unlock(&usb_lock);
  }
}

void pigeon_set_power(int power) {
  pthread_mutex_lock(&usb_lock);
  int err = libusb_control_transfer(dev_handle, 0xc0, 0xd9, power, 0, NULL, 0, TIMEOUT);
  if (err < 0) { handle_usb_issue(err, __func__); }
  pthread_mutex_unlock(&usb_lock);
}

void pigeon_set_baud(int baud) {
  int err;
  pthread_mutex_lock(&usb_lock);
  err = libusb_control_transfer(dev_handle, 0xc0, 0xe2, 1, 0, NULL, 0, TIMEOUT);
  if (err < 0) { handle_usb_issue(err, __func__); }
  err = libusb_control_transfer(dev_handle, 0xc0, 0xe4, 1, baud/300, NULL, 0, TIMEOUT);
  if (err < 0) { handle_usb_issue(err, __func__); }
  pthread_mutex_unlock(&usb_lock);
}

void pigeon_init() {
  usleep(1000*1000);
  LOGW("panda GPS start");

  // power off pigeon
  pigeon_set_power(0);
  usleep(100*1000);

  // 9600 baud at init
  pigeon_set_baud(9600);

  // power on pigeon
  pigeon_set_power(1);
  usleep(500*1000);

  // baud rate upping
  pigeon_send("\x24\x50\x55\x42\x58\x2C\x34\x31\x2C\x31\x2C\x30\x30\x30\x37\x2C\x30\x30\x30\x33\x2C\x34\x36\x30\x38\x30\x30\x2C\x30\x2A\x31\x35\x0D\x0A");
  usleep(100*1000);

  // set baud rate to 460800
  pigeon_set_baud(460800);
  usleep(100*1000);

  // init from ubloxd
  pigeon_send("\xB5\x62\x06\x00\x14\x00\x03\xFF\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x01\x00\x00\x00\x00\x00\x1E\x7F");
  pigeon_send("\xB5\x62\x06\x3E\x00\x00\x44\xD2");
  pigeon_send("\xB5\x62\x06\x00\x14\x00\x00\xFF\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x19\x35");
  pigeon_send("\xB5\x62\x06\x00\x14\x00\x01\x00\x00\x00\xC0\x08\x00\x00\x00\x08\x07\x00\x01\x00\x01\x00\x00\x00\x00\x00\xF4\x80");
  pigeon_send("\xB5\x62\x06\x00\x14\x00\x04\xFF\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x1D\x85");
  pigeon_send("\xB5\x62\x06\x00\x00\x00\x06\x18");
  pigeon_send("\xB5\x62\x06\x00\x01\x00\x01\x08\x22");
  pigeon_send("\xB5\x62\x06\x00\x01\x00\x02\x09\x23");
  pigeon_send("\xB5\x62\x06\x00\x01\x00\x03\x0A\x24");
  pigeon_send("\xB5\x62\x06\x08\x06\x00\x64\x00\x01\x00\x00\x00\x79\x10");
  pigeon_send("\xB5\x62\x06\x24\x24\x00\x05\x00\x04\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x5A\x63");
  pigeon_send("\xB5\x62\x06\x1E\x14\x00\x00\x00\x00\x00\x01\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x3C\x37");
  pigeon_send("\xB5\x62\x06\x24\x00\x00\x2A\x84");
  pigeon_send("\xB5\x62\x06\x23\x00\x00\x29\x81");
  pigeon_send("\xB5\x62\x06\x1E\x00\x00\x24\x72");
  pigeon_send("\xB5\x62\x06\x01\x03\x00\x01\x07\x01\x13\x51");
  pigeon_send("\xB5\x62\x06\x01\x03\x00\x02\x15\x01\x22\x70");
  pigeon_send("\xB5\x62\x06\x01\x03\x00\x02\x13\x01\x20\x6C");

  LOGW("panda GPS on");
}

static void pigeon_publish_raw(void *publisher, unsigned char *dat, int alen) {
  // create message
  capnp::MallocMessageBuilder msg;
  cereal::Event::Builder event = msg.initRoot<cereal::Event>();
  event.setLogMonoTime(nanos_since_boot());
  auto ublox_raw = event.initUbloxRaw(alen);
  memcpy(ublox_raw.begin(), dat, alen);

  // send to ubloxRaw
  auto words = capnp::messageToFlatArray(msg);
  auto bytes = words.asBytes();
  zmq_send(publisher, bytes.begin(), bytes.size(), 0);
}


void *pigeon_thread(void *crap) {
  // ubloxRaw = 8042
  void *context = zmq_ctx_new();
  void *publisher = zmq_socket(context, ZMQ_PUB);
  zmq_bind(publisher, "tcp://*:8042");

  // run at ~100hz
  unsigned char dat[0x1000];
  uint64_t cnt = 0;
  while (!do_exit) {
    if (pigeon_needs_init) {
      pigeon_needs_init = false;
      pigeon_init();
    }

    //pthread_mutex_lock(&usb_lock);	
    //libusb_control_transfer(dev_handle, 0x40, 0xdc, (uint16_t)(cereal::CarParams::SafetyModel::HONDA_BOSCH), 0, NULL, 0, TIMEOUT);	
    //pthread_mutex_unlock(&usb_lock);

    int alen = 0;
    while (alen < 0xfc0) {
      pthread_mutex_lock(&usb_lock);
      int len = libusb_control_transfer(dev_handle, 0xc0, 0xe0, 1, 0, dat+alen, 0x40, TIMEOUT);
      if (len < 0) { handle_usb_issue(len, __func__); }
      pthread_mutex_unlock(&usb_lock);
      if (len <= 0) break;

      //("got %d\n", len);
      alen += len;
    }
    if (alen > 0) {
      if (dat[0] == (char)0x00){
        LOGW("received invalid ublox message, resetting panda GPS");
        pigeon_init();
      } else {
        pigeon_publish_raw(publisher, dat, alen);
      }
    }

    // 10ms
    usleep(30*1000);
    cnt++;
  }

  return NULL;
}

int set_realtime_priority(int level) {
  // should match python using chrt
  struct sched_param sa;
  memset(&sa, 0, sizeof(sa));
  sa.sched_priority = level;
  return sched_setscheduler(getpid(), SCHED_FIFO, &sa);
}

}

int main() {
  int err;
  LOGW("starting boardd");

  // set process priority
  //err = set_realtime_priority(4);
  //LOGW("setpriority returns %d", err);

  // check the environment
  if (getenv("STARTED")) {
    spoofing_started = true;
  }

  if (getenv("FAKESEND")) {
    fake_send = true;
  }

  if (getenv("BOARDD_LOOPBACK")){
    loopback_can = true;
  }

  // init libusb
  err = libusb_init(&ctx);
  assert(err == 0);
  libusb_set_debug(ctx, 3);

  // connect to the board
  usb_retry_connect();


  // create threads
  pthread_t can_health_thread_handle;
  err = pthread_create(&can_health_thread_handle, NULL,
                       can_health_thread, NULL);
  assert(err == 0);

  pthread_t can_send_thread_handle;
  err = pthread_create(&can_send_thread_handle, NULL,
                       can_send_thread, NULL);
  assert(err == 0);

  pthread_t can_recv_thread_handle;
  err = pthread_create(&can_recv_thread_handle, NULL,
                       can_recv_thread, NULL);
  assert(err == 0);

  // join threads

  err = pthread_join(can_recv_thread_handle, NULL);
  assert(err == 0);

  err = pthread_join(can_send_thread_handle, NULL);
  assert(err == 0);

  err = pthread_join(can_health_thread_handle, NULL);
  assert(err == 0);

  //while (!do_exit) usleep(1000);

  // destruct libusb

  libusb_close(dev_handle);
  libusb_exit(ctx);
}
