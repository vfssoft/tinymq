#include <tm.h>
#include "tinyunit.h"
#include "testutil.h"
#include "mymqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mqtt_auth_user_anonymous_cb(void* ctx, tm_t* mq, const char* username, const char* password, int* ret_auth_ok) {
  *ret_auth_ok = 1;
}
static void init_callbacks(tm_callbacks_t* cbs, void* cb_ctx) {
  memset(cbs, 0, sizeof(tm_callbacks_t));
  cbs->cb_ctx = cb_ctx;
  cbs->auth_cb = mqtt_auth_user_anonymous_cb;
}

typedef struct {
  int proto;
  int clean_session;
  char client_id[128];
  char topic[128];
  int qos;
  int timeoutms;
  int exp_recv_count;
  int subscribed;
  int skip_sub;
  int done;
    
  msgs_t* msgs; // msg received
} test_client_subscriber_info_t;

static void mqtt_client_subscriber_cb(void *arg) {
  int err;
  test_client_subscriber_info_t* info = (test_client_subscriber_info_t*) arg;
  mymqtt_t client;
  mymqtt__init(&client, info->proto, info->client_id);

  client.options.cleansession = info->clean_session;

  err = mymqtt__connect(&client);
  ASSERT_EQ(err, 0);

  if (!info->skip_sub) {
    err = mymqtt__subscribe(&client, info->topic, info->qos);
    ASSERT_EQ(err, 0);
  }
  
  info->subscribed = 1;
  
  long long int start = get_current_time_millis();
  while (1) {
    mysleep(20);
    
    if (info->timeoutms > 0) {
      long long int current = get_current_time_millis();
      if (current - start >= info->timeoutms) {
        break;
      }
    }
    if (msgs__count(client.msgs) >= info->exp_recv_count) {
      break;
    }
  }
  
  if (msgs__count(client.msgs) > 0) {
    info->msgs = msgs__clone(client.msgs);
  }

  if (!info->skip_sub) {
    err = mymqtt__unsubscribe(&client, info->topic);
    ASSERT_EQ(err, 0);
  }

  err = mymqtt__disconnect(&client);
  ASSERT_EQ(err, 0);

  info->done = 1;
}


typedef struct {
    int proto;
    char client_id[128];
    char topic[128];
    int qos;
    char* payload;
    int payload_len;
    int retain;
    int done;
} test_client_publisher_info_t;

static void mqtt_client_publisher_cb(void *arg) {
  int err;
  test_client_publisher_info_t* info = (test_client_publisher_info_t*) arg;
  mymqtt_t client;
  mymqtt__init(&client, info->proto, info->client_id);
  
  err = mymqtt__connect(&client);
  ASSERT_EQ(err, 0);
  
  err = mymqtt__publish(&client, info->topic, info->payload, info->payload_len, info->qos, info->retain);
  ASSERT_EQ(err, 0);
  
  err = mymqtt__disconnect(&client);
  ASSERT_EQ(err, 0);
  
  info->done = 1;
}


typedef struct {
    int proto;
    int  clean_session;
    char client_id[128];
    char topic[128];
    int qos;
    int done;
} test_client_connect_sub_info_t;
static void mqtt_client_connect_sub_cb(void *arg) {
  int err;
  test_client_connect_sub_info_t* info = (test_client_connect_sub_info_t*) arg;
  mymqtt_t client;
  mymqtt__init(&client, info->proto, info->client_id);

  client.options.cleansession = info->clean_session;

  err = mymqtt__connect(&client);
  ASSERT_EQ(err, 0);

  if (strlen(info->topic)) {
    err = mymqtt__subscribe(&client, info->topic, info->qos);
    ASSERT_EQ(err, 0);
  }

  err = mymqtt__disconnect(&client);
  ASSERT_EQ(err, 0);

  info->done = 1;
}


static int mqtt_basic_pub_impl(int proto, const char* topic, int qos, char* payload, int payload_len) {
  test_client_publisher_info_t info;
  RESET_STRUCT(info);
  info.proto = proto;
  strcpy(info.client_id, "test_publisher_client_id");
  strcpy(info.topic, topic);
  info.qos = qos;
  info.payload_len = payload_len;
  info.payload = payload;
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, &info);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mythread_t client_thread;
  thread_create(&client_thread, mqtt_client_publisher_cb, (void*)&info);
  
  while (info.done == 0) {
    tm__run(server);
  }
  
  tm__stop(server);
  thread_join(&client_thread);
  
  return 0;
}

TEST_IMPL(mqtt_basic_pub_qos0_test) {
  return mqtt_basic_pub_impl(TS_PROTO_TCP, "a", 0, "hello", 5);
}
TEST_IMPL(mqtt_basic_pub_qos1_test) {
  return mqtt_basic_pub_impl(TS_PROTO_TCP, "a", 1, "hello", 5);
}
TEST_IMPL(mqtt_basic_pub_qos2_test) {
  return mqtt_basic_pub_impl(TS_PROTO_TCP, "a", 2, "hello", 5);
}

static int mqtt_publish_a_msg(tm_t* server, int proto, const char* topic, int qos, char* payload, int payload_len, int retain) {
  test_client_publisher_info_t info;
  RESET_STRUCT(info);
  info.proto = proto;
  strcpy(info.client_id, "test_publisher_client_id");
  strcpy(info.topic, topic);
  info.qos = qos;
  info.payload_len = payload_len;
  info.payload = payload;
  info.retain = retain;
  
  mythread_t publisher_thread;
  thread_create(&publisher_thread, mqtt_client_publisher_cb, (void*)&info);
  while (info.done == 0) { tm__run(server); }
  thread_join(&publisher_thread);
  return 0;
}
static test_client_subscriber_info_t* mqtt_subscriber_start_ex(tm_t* server, mythread_t* thread, int proto, const char* topic, int qos, int timeoutms, const char* client_id, int clean_session, int skip_sub) {
  test_client_subscriber_info_t* info = (test_client_subscriber_info_t*) malloc(sizeof(test_client_subscriber_info_t));
  memset(info, 0, sizeof(test_client_subscriber_info_t));
  info->proto = proto;
  info->clean_session = clean_session;
  strcpy(info->client_id, client_id);
  strcpy(info->topic, topic);
  info->qos = qos;
  info->timeoutms = timeoutms;
  info->exp_recv_count = 1;
  info->skip_sub = skip_sub;
  
  thread_create(thread, mqtt_client_subscriber_cb, (void*)info);
  while (info->subscribed == 0) { tm__run(server); }
  return info;
}
static test_client_subscriber_info_t* mqtt_subscriber_start(tm_t* server, mythread_t* thread, int proto, const char* topic, int qos, int timeoutms) {
  return mqtt_subscriber_start_ex(server, thread, proto, topic, qos, timeoutms, "tet_subscriber_client_id", 1, 0);
}
static int mqtt_subscriber_stop(tm_t* server, mythread_t* thread,  test_client_subscriber_info_t* info) {
  while (info->done == 0) { tm__run(server); }
  thread_join(thread);
  return 0;
}

static int mqtt_basic_pub_recv_impl(
    int proto,
    const char* pub_topic, int pub_qos, char* payload, int payload_len,
    const char* sub_topic, int sub_qos
) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);

  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, sub_topic, sub_qos, 500);
  
  mqtt_publish_a_msg(server, proto, pub_topic, pub_qos, payload, payload_len, 0);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, sub_topic, payload, payload_len, (sub_qos > pub_qos ? pub_qos : sub_qos), 0);
  
  return 0;
}

TEST_IMPL(mqtt_basic_pub_recv_qos0_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 0, "ABC", 3,
      "topic", 0
  );
}
TEST_IMPL(mqtt_basic_pub_recv_qos1_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 1, "ABC", 3,
      "topic", 1
  );
}
TEST_IMPL(mqtt_basic_pub_recv_qos2_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 2, "ABC", 3,
      "topic", 2
  );
}

TEST_IMPL(mqtt_pub_qos_0_sub_qos_1_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 0, "ABC", 3,
      "topic", 1
  );
}
TEST_IMPL(mqtt_pub_qos_0_sub_qos_2_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 0, "ABC", 3,
      "topic", 2
  );
}
TEST_IMPL(mqtt_pub_qos_1_sub_qos_0_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 1, "ABC", 3,
      "topic", 0
  );
}
TEST_IMPL(mqtt_pub_qos_1_sub_qos_2_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 1, "ABC", 3,
      "topic", 2
  );
}
TEST_IMPL(mqtt_pub_qos_2_sub_qos_0_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 2, "ABC", 3,
      "topic", 0
  );
}
TEST_IMPL(mqtt_pub_qos_2_sub_qos_1_tcp) {
  return mqtt_basic_pub_recv_impl(
      TS_PROTO_TCP,
      "topic", 2, "ABC", 3,
      "topic", 1
  );
}

typedef struct {
    int proto;
    char client_id[128];
    char topic[128];
    int qos;
    char* payload;
    int payload_len;
    int retain;
    int disconnect_abnormal;
    int done;
} test_client_will_info_t;
static void mqtt_client_with_will_cb(void *arg) {
  int err;
  test_client_will_info_t* info = (test_client_will_info_t*) arg;
  mymqtt_t client;
  mymqtt__init(&client, info->proto, info->client_id);
  
  mymqtt__set_keep_alive(&client, 1);
  mymqtt__set_will(&client, info->topic, info->qos, info->payload, info->payload_len, info->retain);
  
  err = mymqtt__connect(&client);
  ASSERT_EQ(err, 0);
  
  if (info->disconnect_abnormal) {
    // invalid topic will cause the server disconnect us
    mymqtt__subscribe(&client, "###", 1);
  } else {
    err = mymqtt__disconnect(&client);
    ASSERT_EQ(err, 0);
  }
  
  info->done = 1;
}
static int mqtt_connect_with_will_msg(tm_t* server, int proto, const char* topic, int qos, char* payload, int payload_len, int retain, int disconnect_abnormal) {
  test_client_will_info_t info;
  RESET_STRUCT(info);
  info.proto = proto;
  strcpy(info.client_id, "test_will_client_id");
  strcpy(info.topic, topic);
  info.qos = qos;
  info.payload_len = payload_len;
  info.payload = payload;
  info.retain = retain;
  info.disconnect_abnormal = disconnect_abnormal;
  
  mythread_t publisher_thread;
  thread_create(&publisher_thread, mqtt_client_with_will_cb, (void*)&info);
  while (info.done == 0) { tm__run(server); }
  thread_join(&publisher_thread);
  return 0;
}
static int mqtt_basic_will_msg_impl(int proto, int disconnect_abnormal, int retain) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  const char* will_topic = "will_topic";
  char* payload = "hello will";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, will_topic, 1, 10000);
  
  mqtt_connect_with_will_msg(server, proto, will_topic, 1, payload, strlen(payload), retain, disconnect_abnormal);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  if (disconnect_abnormal) {
    ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
    msg_t* msg = msgs__at(subscriber_info->msgs, 0);
    assert_msg(msg, will_topic, payload, strlen(payload), 1, 0);
  } else {
    ASSERT_EQ(msgs__count(subscriber_info->msgs), 0);
  }
  
  return 0;
}
TEST_IMPL(mqtt_not_pub_will_if_client_disconnect_normally) {
  return mqtt_basic_will_msg_impl(TS_PROTO_TCP, 0, 0);
}
TEST_IMPL(mqtt_pub_will_if_client_disconnect_abnormally) {
  return mqtt_basic_will_msg_impl(TS_PROTO_TCP, 1, 0);
}
TEST_IMPL(mqtt_pub_will_if_client_disconnect_abnormally_retain) {
  return mqtt_basic_will_msg_impl(TS_PROTO_TCP, 1, 1);
}


TEST_IMPL(mqtt_retain_msg_current_subscription) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* topic = "retain_topic";
  int qos = 1;
  char* payload = "hello retain message";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 500);
  
  mqtt_publish_a_msg(server, proto, topic, qos, payload, strlen(payload), 1);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, payload, strlen(payload), qos, 0);
  
  return 0;
}

TEST_IMPL(mqtt_retain_msg_new_subscription) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* topic = "retain_topic";
  int qos = 1;
  char* payload = "hello retain message";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mqtt_publish_a_msg(server, proto, topic, qos, payload, strlen(payload), 1);
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 500);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, payload, strlen(payload), qos, 1);
  
  return 0;
}
TEST_IMPL(mqtt_retain_msg_zero_byte) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* topic = "retain_message_zero_byte_payload";
  int qos = 1;
  char* payload = "";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mqtt_publish_a_msg(server, proto, topic, qos, payload, strlen(payload), 1);
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 1000);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 0);
  
  return 0;
}
TEST_IMPL(mqtt_retain_msg_zero_byte_1) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* topic = "retain_message_zero_byte_payload";
  int qos = 1;
  char* payload = "";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 500);
  
  mqtt_publish_a_msg(server, proto, topic, qos, payload, 0, 1);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, NULL, 0, qos, 0);
  
  return 0;
}
TEST_IMPL(mqtt_retain_msg_zero_byte_2) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* topic = "retain_message_zero_byte_payload";
  int qos = 1;
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mqtt_publish_a_msg(server, proto, topic, qos, "A", 1, 1); // create a retained message
  mqtt_publish_a_msg(server, proto, topic, qos, "", 0, 1); // remove the retained message
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 1000);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 0);
  
  return 0;
}
TEST_IMPL(mqtt_retain_msg_zero_byte_3) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* topic = "retain_message_zero_byte_payload";
  int qos = 1;
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mqtt_publish_a_msg(server, proto, topic, qos, "A", 1, 1); // create a retained message
  mqtt_publish_a_msg(server, proto, topic, qos, "", 0, 0); // public a message with the same topic but not retained
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 500);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, "A", 1, qos, 1);
  
  return 0;
}
TEST_IMPL(mqtt_retain_msg_update_exist) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* topic = "retain_message_topic";
  int qos = 1;
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mqtt_publish_a_msg(server, proto, topic, qos, "A", 1, 1); // create a retained message
  mqtt_publish_a_msg(server, proto, topic, qos, "B", 1, 1); // update the retained message
  
  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 500);
  
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, "B", 1, qos, 1);
  
  return 0;
}

TEST_IMPL(mqtt_retain_msg_kept_after_publisher_session_ends) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;

  int proto = TS_PROTO_TCP;
  const char* topic = "retain_message_topic";
  int qos = 1;

  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);

  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);

  // clean session is 1
  mqtt_publish_a_msg(server, proto, topic, qos, "A", 1, 1);

  subscriber_info = mqtt_subscriber_start(server, &subscriber_thread, proto, topic, qos, 500);

  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);

  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, "A", 1, qos, 1);

  return 0;
}



static int mqtt_connect_and_sub(tm_t* server, int proto, int clean_session, const char* client_id, const char* topic, int qos) {
  test_client_connect_sub_info_t info;
  RESET_STRUCT(info);
  info.proto = proto;
  info.clean_session = clean_session;
  strcpy(info.client_id, client_id);
  strcpy(info.topic, topic);
  info.qos = qos;

  mythread_t conn_sub_thread;
  thread_create(&conn_sub_thread, mqtt_client_connect_sub_cb, (void*)&info);
  while (info.done == 0) { tm__run(server); }
  thread_join(&conn_sub_thread);
  return 0;
}

TEST_IMPL(mqtt_recv_offline_msgs_after_reconnect) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;

  int proto = TS_PROTO_TCP;
  const char* client_id = "subscriber_with_clean_session";
  const char* topic = "topic";
  int qos = 1;
  char* payload = "hello offline message";

  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);

  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);

  mqtt_connect_and_sub(server, proto, 0, client_id, topic, qos);

  mqtt_publish_a_msg(server, proto, topic, qos, payload, strlen(payload), 0);

  subscriber_info = mqtt_subscriber_start_ex(server, &subscriber_thread, proto, topic, qos, 500, client_id, 0, 1);
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);

  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, payload, strlen(payload), qos, 0);

  return 0;
}

TEST_IMPL(mqtt_no_offline_msgs_after_reconnect_with_clean_session) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;

  int proto = TS_PROTO_TCP;
  const char* client_id = "subscriber_with_clean_session";
  const char* topic = "topic";
  int qos = 1;
  char* payload = "hello offline message";

  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);

  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);

  mqtt_connect_and_sub(server, proto, 0, client_id, topic, qos);

  mqtt_publish_a_msg(server, proto, topic, qos, payload, strlen(payload), 0);

  subscriber_info = mqtt_subscriber_start_ex(server, &subscriber_thread, proto, topic, qos, 500, client_id, 1, 1);
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);

  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 0);

  return 0;
}

TEST_IMPL(mqtt_max_qos_of_all_subscriptions) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* client_id = "subscriber_with_multiple_subscriptions";
  const char* topic = "/test/a/topic";
  int qos = 1;
  char* payload = "hello message";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mqtt_connect_and_sub(server, proto, 0, client_id, "/test/a/+", 1);
  mqtt_connect_and_sub(server, proto, 0, client_id, "/test/+", 0);
  
  mqtt_publish_a_msg(server, proto, topic, 2, payload, strlen(payload), 0);
  
  subscriber_info = mqtt_subscriber_start_ex(server, &subscriber_thread, proto, topic, qos, 500, client_id, 0, 1);
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, payload, strlen(payload), 1, 0);
  
  return 0;
}


TEST_IMPL(mqtt_update_subscribe_qos) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* client_id = "subscriber";
  const char* topic = "/test/a/topic";
  char* payload = "hello message";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  mqtt_connect_and_sub(server, proto, 0, client_id, topic, 0);
  subscriber_info = mqtt_subscriber_start_ex(server, &subscriber_thread, proto, topic, 1, 500, client_id, 0, 0); // re-sub with qos = 1
  mqtt_publish_a_msg(server, proto, topic, 2, payload, strlen(payload), 0);
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  tm__stop(server);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  assert_msg(msg, topic, NULL, -1, 1, 0);
  
  return 0;
}

TEST_IMPL(mqtt_update_subscribe_qos_resent_retain_msg) {
  test_client_subscriber_info_t* subscriber_info;
  mythread_t subscriber_thread;
  
  int proto = TS_PROTO_TCP;
  const char* client_id = "subscriber";
  const char* topic = "/test/a/topic";
  char* payload = "hello message";
  
  tm_t* server;
  tm_callbacks_t cbs;
  init_callbacks(&cbs, NULL);
  
  server = start_mqtt_server(proto, &cbs);
  int r = tm__start(server);
  ASSERT_EQ(r, 0);
  
  subscriber_info = mqtt_subscriber_start_ex(server, &subscriber_thread, proto, topic, 0, 500, client_id, 0, 0);
  mqtt_publish_a_msg(server, proto, topic, 2, payload, strlen(payload), 1);
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg_t* msg = msgs__at(subscriber_info->msgs, 0);
  ASSERT_EQ(msg->qos, 0);
  
  subscriber_info = mqtt_subscriber_start_ex(server, &subscriber_thread, proto, topic, 1, 500, client_id, 0, 0);
  mqtt_subscriber_stop(server, &subscriber_thread, subscriber_info);
  
  ASSERT_EQ(msgs__count(subscriber_info->msgs), 1);
  msg = msgs__at(subscriber_info->msgs, 0);
  ASSERT_EQ(msg->qos, 1);
  
  tm__stop(server);

  return 0;
}

