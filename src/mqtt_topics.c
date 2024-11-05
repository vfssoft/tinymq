#include "mqtt_topics.h"

#include <internal/ts_mem.h>
#include <internal/utlist.h>

void tm_matched_subscribers__destroy(tm_matched_subscriber_t* subscribers) {
  tm_matched_subscriber_t* elem;
  tm_matched_subscriber_t* tmp;
  
  HASH_ITER(hh, subscribers, elem, tmp) {
    HASH_DEL(subscribers, elem);
  
    assert(elem->qoss);
    ts_int_arr__destroy(elem->qoss);
    ts__free(elem);
  
  }
}
int tm_matched_subscribers__count(tm_matched_subscriber_t* subscribers) {
  return HASH_COUNT(subscribers);
}
static int tm_matched_subscribers__add(tm_matched_subscriber_t** subscribers, void* subscriber, int qos) {
  tm_matched_subscriber_t* val = NULL;
  
  HASH_FIND_PTR(*subscribers, &subscriber, val);
  
  if (val == NULL) {
    val = (tm_matched_subscriber_t*) ts__malloc(sizeof(tm_matched_subscriber_t));
    if (val == NULL) {
      return TS_ERR_OUT_OF_MEMORY;
    }
    
    val->subscriber = subscriber;
    val->qoss = ts_int_arr__create(0);
    if (val->qoss == NULL) {
      return TS_ERR_OUT_OF_MEMORY;
    }
    
    HASH_ADD_PTR(*subscribers, subscriber, val);
  }
  
  ts_int_arr__append(val->qoss, qos);
  
  return 0;
}


static tm_subscribers_t* create_subscriber(int qos, void* subscriber) {
  tm_subscribers_t* sub = (tm_subscribers_t*) ts__malloc(sizeof(tm_subscribers_t));
  if (sub == NULL) {
    return NULL;
  }
  memset(sub, 0, sizeof(tm_subscribers_t));
  sub->subscriber = subscriber;
  sub->qos = qos;
  
  return sub;
}
static void destroy_subscriber(tm_subscribers_t* sub) {
  ts__free(sub);
}

static tm_topic_node_t* tm_topic_node__find_child_by_name(tm_topic_node_t* parent, const char* name, int name_len) {
  tm_topic_node_t* cur = NULL;
  DL_FOREACH(parent->children, cur) {
    // case sensitive
    if (strncmp(cur->name, name, name_len) == 0) {
      return cur;
    }
  }
  return cur;
}
static tm_topic_node_t* tm_topic_node__add_child(tm_topic_node_t* parent, const char* name, int name_len) {
  tm_topic_node_t* child;
  
  child = (tm_topic_node_t*) ts__malloc(sizeof(tm_topic_node_t));
  if (child == NULL) {
    return NULL;
  }
  memset(child, 0, sizeof(tm_topic_node_t));
  
  child->name = (char*) ts__malloc(name_len + 1);
  if (child->name == NULL) {
    return NULL;
  }
  
  child->parent = parent;
  strncpy(child->name, name, name_len);
  child->name[name_len] = 0;
  
  DL_APPEND(parent->children, child);
  
  return child;
}
static void tm_topic_node__remove_child(tm_topic_node_t* parent, tm_topic_node_t* child) {
  DL_DELETE(parent->children, child);
  
  if (child->name) {
    ts__free(child->name);
  }
  
  ts__free(child);
}
static int tm_topic_node__child_count(tm_topic_node_t* n) {
  int count = 0;
  tm_topic_node_t* child = NULL;
  
  DL_COUNT(n->children, child, count);
  
  return count;
}
static int tm_topic_node__subscribers_count(tm_topic_node_t* n) {
  int count;
  tm_subscribers_t* subscriber = NULL;
  
  DL_COUNT(n->subscribers, subscriber, count);
  
  return count;
}
static void tm_topic_node__remove_subscriber(tm_topic_node_t* n, tm_subscribers_t* sub) {
  DL_DELETE(n->subscribers, sub);
  ts__free(sub);
}
static int tm_topic_node__get_subscribers(tm_topic_node_t* n, int include_children, tm_matched_subscriber_t** subscribers) {
  int err = 0;
  tm_subscribers_t* sub = NULL;
  tm_subscribers_t* sub_copy = NULL;
  tm_topic_node_t* child_node = NULL;
  
  // Implementation Note:
  // Always returns a copy of subscribers for simply
  // If it causes a performance issues, we can refactor it
  //   If we do that, we need to ensure the memory is safely locked
  
  DL_FOREACH(n->subscribers, sub) {
    err = tm_matched_subscribers__add(subscribers, sub->subscriber, sub->qos);
    if (err) {
      return err;
    }
  }
  
  if (include_children) {
    DL_FOREACH(n->children, child_node) {
      err = tm_topic_node__get_subscribers(child_node, 1, subscribers);
      if (err) {
        return err;
      }
    }
  }
  
  return 0;
}
static int tm_topic_node__empty(tm_topic_node_t* n) {
  return
      tm_topic_node__subscribers_count(n) == 0 &&
      tm_topic_node__child_count(n) &&
      n->retained_msg == NULL;
}

static int tm_topic_node__get_by_topic(tm_topic_node_t* n, const char* topic, int create_if_not_exist, tm_topic_node_t** found_node) {
  tm_topic_node_t* child = NULL;
  int start = 0;
  int end = 0;
  
  *found_node = NULL; // not found
  
  while (1) {
    char c = topic[end];
    if (c != TP_LEVEL_SEPARATOR && c != 0) {
      end++;
    } else {
      const char* level = topic + start;
      int level_len = end - start;
      
      child = tm_topic_node__find_child_by_name(n, level, level_len);
      if (child == NULL) {
        if (create_if_not_exist) {
          child = tm_topic_node__add_child(n, level, level_len);
          if (child == NULL) {
            return TS_ERR_OUT_OF_MEMORY;
          }
        } else {
          return TS_ERR_NOT_FOUND;
        }
      }
      n = child;
      end++;
      start = end;
      
      if (c == 0) {
        break;
      }
    }
  }
  
  *found_node = n;
  return 0;
}
static int tm_topic_node__free_empty_nodes(tm_topic_node_t* node) {
  while (node != NULL && tm_topic_node__empty(node)) {
    tm_topic_node_t* parent = node->parent;
    tm_topic_node__remove_child(parent, node);
    node = parent;
  }
  return 0;
}

static int tm_topic_node__insert(tm_topic_node_t* n, const char* topic, char qos, void* subscriber) {
  int err;
  tm_topic_node_t* child = NULL;
  tm_subscribers_t* sub = NULL;
  
  err = tm_topic_node__get_by_topic(n, topic, 1, &child);
  if (err) {
    return err;
  }
  n = child;
  
  DL_FOREACH(n->subscribers, sub) {
    if (sub->subscriber == subscriber) {
      sub->qos = qos;
      return 0;
    }
  }
  
  sub = create_subscriber(qos, subscriber);
  if (sub == NULL) {
    return TS_ERR_OUT_OF_MEMORY;
  }
  DL_APPEND(n->subscribers, sub);
  return 0;
}
static int tm_topic_node__remove(tm_topic_node_t* n, const char* topic, void* subscriber) {
  int err;
  tm_topic_node_t* child = NULL;
  tm_subscribers_t* sub = NULL;
  tm_subscribers_t* tmp_sub = NULL;
  int removed = 0;
  
  err = tm_topic_node__get_by_topic(n, topic, 0, &child);
  if (err) {
    return err;
  }
  n = child;
  
  if (subscriber == NULL) {
    // it's signal to remove ALL subscribers
    DL_FOREACH_SAFE(n->subscribers, sub, tmp_sub) {
      tm_topic_node__remove_subscriber(n, sub);
      removed = 1;
    }
  } else {
    DL_FOREACH(n->subscribers, sub) {
      if (sub->subscriber == subscriber) {
        tm_topic_node__remove_subscriber(n, sub);
        removed = 1;
        break;
      }
    }
  }
  if (!removed) {
    return TS_ERR_NOT_FOUND; // no topic found
  }
  
  return tm_topic_node__free_empty_nodes(n);
}
static int tm_topic_node__match(tm_topic_node_t* n, const char* topic, tm_matched_subscriber_t** subscribers) {
  int err = 0;
  const char* level = topic;
  int level_len = 0;
  tm_topic_node_t* child = NULL;
  tm_subscribers_t* sub = NULL;
  
  // level == NULL, we matched whole topic
  // level != NULL && level[0] == 0, we have a matched parent
  if (level == NULL/* || level[0] == 0*/) {
    err = tm_topic_node__get_subscribers(n, 0, subscribers);
    if (err) {
      return err;
    }
  
    // Check # children
    // For example: "sport/tennis/player1/#" matches "sport/tennis/player1"
    DL_FOREACH(n->children, child) {
      if (strlen(child->name) == 1 && child->name[0] == TP_MULTI_LEVEL_WILDCARD) {
        err = tm_topic_node__get_subscribers(child, 1, subscribers);
        if (err) {
          return err;
        }
        break;
      }
    }
    return 0;
  }
  
  // find next topic level
  while (level[level_len] != TP_LEVEL_SEPARATOR && level[level_len] != '\0') {
    level_len++;
  }
  
  DL_FOREACH(n->children, child) {
    if (strlen(child->name) == 1 && child->name[0] == TP_MULTI_LEVEL_WILDCARD) {
      err = tm_topic_node__get_subscribers(child, 1, subscribers);
      if (err) {
        return err;
      }
    } else if ((strlen(child->name) == 1 && child->name[0] == TP_SINGLE_LEVEL_WILDCARD) || strncmp(level, child->name, level_len) == 0) {
      err = tm_topic_node__match(
          child,
          level[level_len] == '\0' ? NULL : level + level_len + 1,
          subscribers);
      if (err) {
        return err;
      }
    }
  }
  
  return 0;
}


static int tm_topic_node__get_retained_msgs(tm_topic_node_t* n, int include_children, ts_ptr_arr_t* retained_msgs) {
  int err = 0;
  tm_topic_node_t* child_node;
  
  if (n->retained_msg) {
    err = ts_ptr_arr__append(retained_msgs, n->retained_msg);
    if (err) {
      return err;
    }
  }
  
  if (include_children) {
    DL_FOREACH(n->children, child_node) {
      err = tm_topic_node__get_retained_msgs(child_node, 1, retained_msgs);
      if (err) {
        return err;
      }
    }
  }
  
  return 0;
}
static int tm_topic_node__insert_retain_msg(tm_topic_node_t* n, const char* topic, tm_mqtt_msg_t* msg, tm_mqtt_msg_t** removed_retained_msg) {
  int err;
  tm_topic_node_t* child = NULL;
  
  err = tm_topic_node__get_by_topic(n, topic, 1, &child);
  if (err) {
    return err;
  }

  if (removed_retained_msg) *removed_retained_msg = child->retained_msg;
  child->retained_msg = msg;
  return 0;
}
static int tm_topic_node__remove_retain_msg(tm_topic_node_t* n, const char* topic, tm_mqtt_msg_t** removed_retained_msg) {
  int err;
  tm_topic_node_t* child = NULL;
  
  err = tm_topic_node__get_by_topic(n, topic, 0, &child);
  if (err) {
    return err;
  }
  n = child;

  if (removed_retained_msg) *removed_retained_msg = n->retained_msg;
  if (n->retained_msg) {
    n->retained_msg = NULL;
  } else {
    return TS_ERR_NOT_FOUND; // no topic found
  }
  
  return tm_topic_node__free_empty_nodes(n);
}
static int tm_topic_node__match_retain_msgs(tm_topic_node_t* n, const char* topic, ts_ptr_arr_t* retained_msgs) {
  int err = 0;
  const char* level = topic;
  int level_len = 0;
  tm_topic_node_t* child = NULL;
  
  // level == NULL, we matched whole topic
  // level != NULL && level[0] == 0, we have a matched parent
  if (level == NULL/* || level[0] == 0*/) {
    err = tm_topic_node__get_retained_msgs(n, 0, retained_msgs);
    if (err) {
      return err;
    }
    
    // Check # children
    // For example: "sport/tennis/player1/#" matches "sport/tennis/player1"
    DL_FOREACH(n->children, child) {
      if (strlen(child->name) == 1 && child->name[0] == TP_MULTI_LEVEL_WILDCARD) {
        err = tm_topic_node__get_retained_msgs(child, 1, retained_msgs);
        if (err) {
          return err;
        }
        break;
      }
    }
    return 0;
  }
  
  // find next topic level
  while (level[level_len] != TP_LEVEL_SEPARATOR && level[level_len] != '\0') {
    level_len++;
  }
  
  DL_FOREACH(n->children, child) {
    if (level_len == 1 && level[0] == TP_MULTI_LEVEL_WILDCARD) {
      err = tm_topic_node__get_retained_msgs(child, 1, retained_msgs);
      if (err) {
        return err;
      }
    } else if ((level_len == 1 && level[0] == TP_SINGLE_LEVEL_WILDCARD) || strncmp(level, child->name, level_len) == 0) {
      err = tm_topic_node__match_retain_msgs(
          child,
          level[level_len] == '\0' ? NULL : level + level_len + 1,
          retained_msgs
      );
      if (err) {
        return err;
      }
    }
  }
  
  return ts_ptr_arr__get_count(retained_msgs) > 0 ? 0 : TS_ERR_NOT_FOUND;
}

tm_topics_t* tm_topics__create() {
  tm_topics_t* t = (tm_topics_t*) ts__malloc(sizeof(tm_topics_t));
  if (t == NULL) {
    return NULL;
  }

  t->mu = ts_mutex__create();
  ts_error__init(&(t->err));
  
  memset(&(t->root), 0, sizeof(tm_topic_node_t));
  
  return t;
}
int tm_topics__destroy(tm_topics_t* t) {
  ts_mutex__destroy(t->mu);
  
  // TODO: free sub_root
  return 0;
}

int tm_topics__subscribe(tm_topics_t* t, const char* topic, char qos, void* subscriber) {
  int err;
  ts_mutex__lock(t->mu);
  
  err = tm_topic_node__insert(&(t->root), topic, qos, subscriber);
  if (err) {
    ts_error__set(&(t->err), err);
  }
  
  ts_mutex__unlock(t->mu);
  
  return err;
}
int tm_topics__unsubscribe(tm_topics_t* t, const char* topic, void* subscriber) {
  int err;
  ts_mutex__lock(t->mu);
  
  
  err = tm_topic_node__remove(&(t->root), topic, subscriber);
  if (err) {
    ts_error__set(&(t->err), err);
  }
  
  ts_mutex__unlock(t->mu);
  
  return err;
}
int tm_topics__subscribers(tm_topics_t* t, const char* topic, char qos, tm_matched_subscriber_t** subscribers) {
  int err;
  ts_mutex__lock(t->mu);
  
  
  err = tm_topic_node__match(&(t->root), topic, subscribers);
  if (err) {
    ts_error__set(&(t->err), err);
  }
  
  ts_mutex__unlock(t->mu);
  
  return err;
}
int tm_topics__subscribers_free(tm_subscribers_t* subscribers) {
  tm_subscribers_t* tmp;
  tm_subscribers_t* cur;
  DL_FOREACH_SAFE(subscribers, cur, tmp) {
    destroy_subscriber(cur);
  }
  return 0;
}

int tm_topics__retain_msg(tm_topics_t* t, tm_mqtt_msg_t* msg, tm_mqtt_msg_t** removed_retained_msg) {
  int err;
  ts_mutex__lock(t->mu);
  
  if (msg->msg_core->payload->len == 0) { // remove retained message
    err = tm_topic_node__remove_retain_msg(&(t->root), msg->msg_core->topic->buf, removed_retained_msg);
  } else {
    err = tm_topic_node__insert_retain_msg(&(t->root), msg->msg_core->topic->buf, msg, removed_retained_msg);
  }
  
  if (err) {
    ts_error__set(&(t->err), err);
  }
  
  ts_mutex__unlock(t->mu);
  return err;
}
int tm_topics__get_retained_msgs(tm_topics_t* t, const char* topic, ts_ptr_arr_t* retained_msgs) {
  int err;
  ts_mutex__lock(t->mu);
  
  err = tm_topic_node__match_retain_msgs(&(t->root), topic, retained_msgs);
  if (err) {
    ts_error__set(&(t->err), err);
  }
  
  ts_mutex__unlock(t->mu);
  return err;
}


static int tm_topics__valid_common(const char* topic, int topic_len, ts_error_t* err) {
  if (topic == NULL || topic_len == 0) {
    ts_error__set_msg(err, TS_ERR_INVALID_TOPIC, "Topic MUST be at least one character long");
    return err->err;
  }
  
  if (topic_len > 65535) {
    ts_error__set_msg(err, TS_ERR_INVALID_TOPIC, "Topic MUST NOT encode to more than 65535 bytes");
    return err->err;
  }
  
  for (int i = 0; i < topic_len; i++) {
    if (topic[i] == 0) {
      ts_error__set_msg(err, TS_ERR_INVALID_TOPIC, "Topic MUST NOT contains NULL");
      return err->err;
    }
  }
  
  return 0;
}
int tm_topics__valid_topic_filter(const char* topic, int topic_len, ts_error_t* err) {
  tm_topics__valid_common(topic, topic_len, err);
  if (err->err) {
    return err->err;
  }
  
  for (int i = 0; i < topic_len; i++) {
    char c = topic[i];
    
    if (c == TP_MULTI_LEVEL_WILDCARD) {
      if (i + 1 != topic_len) {
        ts_error__set_msg(err, TS_ERR_INVALID_TOPIC, "Multi-level wildcard MUST be the last character in the topic");
        return TS_ERR_INVALID_TOPIC;
      }
      
      if (i > 0 && topic[i-1] != TP_LEVEL_SEPARATOR) {
        ts_error__set_msg(err, TS_ERR_INVALID_TOPIC, "Multi-level wildcard can only follow a topic level separator");
        return TS_ERR_INVALID_TOPIC;
      }
    } else if (c == TP_SINGLE_LEVEL_WILDCARD) {
      if ((i > 0 && topic[i-1] != TP_LEVEL_SEPARATOR) || (i + 1 < topic_len && topic[i+1] != TP_LEVEL_SEPARATOR)) {
        ts_error__set_msg(err, TS_ERR_INVALID_TOPIC, "Single-level wildcard MUST occupy an entire level of the topic filter");
        return TS_ERR_INVALID_TOPIC;
      }
    }
  }
  
  return 0;
}
int tm_topics__valid_topic_name(const char* topic, int topic_len, ts_error_t* err) {
  tm_topics__valid_common(topic, topic_len, err);
  if (err->err) {
    return err->err;
  }
  
  for (int i = 0; i < topic_len; i++) {
    char c = topic[i];
    if (c == TP_MULTI_LEVEL_WILDCARD || c == TP_SINGLE_LEVEL_WILDCARD) {
      ts_error__set_msgf(err, TS_ERR_INVALID_TOPIC, "Invalid characters in the topic(%d)", c);
      return err->err;
    }
  }
  
  return 0;
}
