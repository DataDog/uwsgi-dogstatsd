#include <uwsgi.h>
#include <stdbool.h>

#define MAX_BUFFER_SIZE 8192
#define MAX_CUSTOM_TAGS 10

/*

this is a stats pusher plugin for DogStatsD:

--stats-push dogstatsd:address[,prefix][;custom_tag_name:custom_tag_value...]

example:

--stats-push dogstatsd:127.0.0.1:8125,myinstance,custom_tag_name:custom_tag_value,custom_tag2_name:custom_tag2_value

exports values exposed by the metric subsystem to a Datadog Agent StatsD server

*/

extern struct uwsgi_server uwsgi;

// configuration of a dogstatsd node
struct dogstatsd_node {
  int fd;
  union uwsgi_sockaddr addr;
  socklen_t addr_len;
  char *prefix;
  uint16_t prefix_len;
  char *custom_tags[MAX_CUSTOM_TAGS];
  uint16_t custom_tag_lens[MAX_CUSTOM_TAGS];
};

static int dogstatsd_generate_tags(char *metric, size_t metric_len, char *datatog_metric_name, char *datadog_tags) {
  char *start = metric;
  size_t metric_offset = 0;

  static char metric_separator[] = ".";
  static char tag_separator[] = ",";
  static char tag_colon = ':';
  static char tag_prefix[] = "|#";

  long string_to_int;
  char *token = NULL;
  char *ctxt = NULL;
  char *key = NULL;
  char *next_character = NULL;

  errno = 0;

  token = strtok_r(start, metric_separator, &ctxt);

  if (!token)
    return -1;

  while (token != NULL && metric_len >= metric_offset) {

    metric_offset += strlen(token) + 1;
    start = metric + metric_offset;

    // try to convert token into integer
    string_to_int = strtol(token, &next_character, 10);

    // stop processing if string_to_int is out of range
    if ((string_to_int == LONG_MIN || string_to_int == LONG_MAX) && errno == ERANGE)
      return -1;

    // if we've got a number and a tag value:
    if (next_character != token && key) {

      // start with tag_separator if we already have some tags
      //   otherwise put the tag_prefix
      if (strlen(datadog_tags))
       strncat(datadog_tags, tag_separator, (MAX_BUFFER_SIZE - strlen(datadog_tags) - strlen(tag_separator) - 1));
      else
       strncat(datadog_tags, tag_prefix, (MAX_BUFFER_SIZE - strlen(datadog_tags) - strlen(tag_prefix) - 1));

      // append new tag
      strncat(datadog_tags, key, (MAX_BUFFER_SIZE - strlen(datadog_tags) - strlen(key) - 1));
      strncat(datadog_tags, &tag_colon, 1);
      strncat(datadog_tags, token, (MAX_BUFFER_SIZE - strlen(datadog_tags) - strlen(token) - 1));

    } else {

      // store token as a key for the next iteration
      key = token;

      // start with metric_separator if we already have some metrics
      if (strlen(datatog_metric_name))
       strncat(datatog_metric_name, metric_separator, (MAX_BUFFER_SIZE - strlen(datatog_metric_name) - strlen(metric_separator) - 1));

      // add token
      strncat(datatog_metric_name, token, (MAX_BUFFER_SIZE - strlen(datatog_metric_name) - strlen(token) - 1));
    }

    // try to generate tokens before we iterate again
    token = strtok_r(NULL, metric_separator, &ctxt);
  }

  return strlen(datatog_metric_name);
}


static int dogstatsd_send_metric(struct uwsgi_buffer *ub, struct uwsgi_stats_pusher_instance *uspi, char *metric, size_t metric_len, int64_t value, char type[2]) {
  struct dogstatsd_node *sn = (struct dogstatsd_node *) uspi->data;

  char datatog_metric_name[MAX_BUFFER_SIZE];
  char datadog_tags[MAX_BUFFER_SIZE];
  char raw_metric_name[MAX_BUFFER_SIZE];
  char custom_tags_str[MAX_BUFFER_SIZE];

  int extracted_tags = 0;

  // check if we can handle such a metric length
  if (metric_len >= MAX_BUFFER_SIZE)
    return -1;

  // reset the buffer
  ub->pos = 0;

  // sanitize buffers
  memset(datadog_tags, 0, MAX_BUFFER_SIZE);
  memset(custom_tags_str, 0, MAX_BUFFER_SIZE);
  memset(datatog_metric_name, 0, MAX_BUFFER_SIZE);

  // let's copy original metric name before we start
  strncpy(raw_metric_name, metric, metric_len + 1);

  // try to extract tags
  extracted_tags = dogstatsd_generate_tags(raw_metric_name, metric_len, datatog_metric_name, datadog_tags);

  if (extracted_tags < 0)
    return -1;

  if (uwsgi_buffer_append(ub, sn->prefix, sn->prefix_len)) return -1;
  if (uwsgi_buffer_append(ub, ".", 1)) return -1;

  // put the datatog_metric_name if we found some tags
  if (extracted_tags) {
    if (uwsgi_buffer_append(ub, datatog_metric_name, strlen(datatog_metric_name))) return -1;
  } else {
    if (uwsgi_buffer_append(ub, metric, strlen(metric))) return -1;
  }

  if (uwsgi_buffer_append(ub, ":", 1)) return -1;
  if (uwsgi_buffer_num64(ub, value)) return -1;
  if (uwsgi_buffer_append(ub, type, 2)) return -1;

  // add tags metadata if there are any
  if (extracted_tags) {
    if (uwsgi_buffer_append(ub, datadog_tags, strlen(datadog_tags))) return -1;
  }
  // add any custom tags
  if (sn->custom_tags[0] != NULL){
    if (strlen(datadog_tags) > 0) {
      strcpy(custom_tags_str, ",");
    } else {
      strcpy(custom_tags_str, "|#");
    }
    strcat(custom_tags_str, sn->custom_tags[0]);
    int i;
    for (i=1; i<MAX_CUSTOM_TAGS; ++i) {
      if (sn->custom_tags[i] == NULL){
        break;
      }
      strncat(custom_tags_str, ",", 1);
      strncat(custom_tags_str, sn->custom_tags[i], sn->custom_tag_lens[i]);
    }
    if (uwsgi_buffer_append(ub, custom_tags_str, strlen(custom_tags_str))) return -1;
  }

  if (sendto(sn->fd, ub->buf, ub->pos, 0, (struct sockaddr *) &sn->addr.sa_in, sn->addr_len) < 0) {
    uwsgi_error("dogstatsd_send_metric()/sendto()");
  }

  return 0;
}


static void stats_pusher_dogstatsd(struct uwsgi_stats_pusher_instance *uspi, time_t now, char *json, size_t json_len) {

  if (!uspi->configured) {
    struct dogstatsd_node *sn = uwsgi_calloc(sizeof(struct dogstatsd_node));

    char *comma = strchr(uspi->arg, ',');
    char *pipe = strchr(uspi->arg, '|');
    bool has_prefix = false;
    bool has_custom_tags = false;
    int num_custom_tags = 0;
    if (comma) {
        if (pipe) {
            has_custom_tags = true;
            if ((comma - uspi->arg) < (pipe - uspi->arg)) {
                has_prefix = true;
            }
        } else {
            has_prefix = true;
        }
    } else {
        if (pipe) {
            has_custom_tags = true;
        }
    }
    char* token;

    if (has_prefix) {
        if (has_custom_tags) {
            token = strtok(comma+1, "|");
        } else {
            token = comma+1;
        }
        sn->prefix = token;
        sn->prefix_len = strlen(token);
        *comma = 0;
    }
    else {
      sn->prefix = "uwsgi";
      sn->prefix_len = 5;
    }

    if (has_custom_tags) {
        token = strtok(pipe+2, ","); // +2 to remove the #
        while (token != NULL) {
            sn->custom_tags[num_custom_tags] = token;
            sn->custom_tag_lens[num_custom_tags] = strlen(token);
            num_custom_tags += 1;
            token = strtok(NULL, ",");
        }
    }

    char *colon = strchr(uspi->arg, ':');
    if (!colon) {
      uwsgi_log("invalid dd address %s\n", uspi->arg);
      if (comma) *comma = ',';
      free(sn);
      return;
    }
    sn->addr_len = socket_to_in_addr(uspi->arg, colon, 0, &sn->addr.sa_in);

    sn->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sn->fd < 0) {
      uwsgi_error("stats_pusher_dogstatsd()/socket()");
      if (comma) *comma = ',';
                        free(sn);
                        return;
    }
    uwsgi_socket_nb(sn->fd);
    if (comma) *comma = ',';
    uspi->data = sn;
    uspi->configured = 1;
  }

  // we use the same buffer for all of the packets
  struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);
  struct uwsgi_metric *um = uwsgi.metrics;
  while(um) {
    uwsgi_rlock(uwsgi.metrics_lock);
    // ignore return value
    if (um->type == UWSGI_METRIC_GAUGE) {
      dogstatsd_send_metric(ub, uspi, um->name, um->name_len, *um->value, "|g");
    }
    else {
      dogstatsd_send_metric(ub, uspi, um->name, um->name_len, *um->value, "|c");
    }
    uwsgi_rwunlock(uwsgi.metrics_lock);
    if (um->reset_after_push){
      uwsgi_wlock(uwsgi.metrics_lock);
      *um->value = um->initial_value;
      uwsgi_rwunlock(uwsgi.metrics_lock);
    }
    um = um->next;
  }
  uwsgi_buffer_destroy(ub);
}

static void dogstatsd_init(void) {
        struct uwsgi_stats_pusher *usp = uwsgi_register_stats_pusher("dogstatsd", stats_pusher_dogstatsd);
  // we use a custom format not the JSON one
  usp->raw = 1;
}

struct uwsgi_plugin dogstatsd_plugin = {

        .name = "dogstatsd",
        .on_load = dogstatsd_init,
};
