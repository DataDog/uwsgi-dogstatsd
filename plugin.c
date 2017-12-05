#include <uwsgi.h>

#define MAX_BUFFER_SIZE 8192

/*

this is a stats pusher plugin for DogStatsD:

--stats-push dogstatsd:address[,prefix]

example:

--stats-push dogstatsd:127.0.0.1:8125,myinstance
--dogstatsd-tags service_name:frontend_service,environment:staging

exports values exposed by the metric subsystem to a Datadog Agent StatsD server

*/

extern struct uwsgi_server uwsgi;

// configuration of a dogstatsd stats pusher node
struct dogstatsd_node {
  int fd;
  union uwsgi_sockaddr addr;
  socklen_t addr_len;
  char *prefix;
  uint16_t prefix_len;
};

// configuration of dogstatsd
static struct dogstatsd_config {
  struct uwsgi_string_list *tags;
} config;

static struct uwsgi_option dogstatsd_options[] = {
    {"dogstatsd-tags", no_argument, 0, "comma-separated list of tags to send with metrics", uwsgi_opt_add_string_list, &config.tags, 0},

    {0, 0, 0, 0, 0, 0, 0},
};

static int dogstatsd_generate_tags(char *metric, size_t metric_len, char *datadog_metric_name, char *datadog_tags) {
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
      if (strlen(datadog_metric_name))
       strncat(datadog_metric_name, metric_separator, (MAX_BUFFER_SIZE - strlen(datadog_metric_name) - strlen(metric_separator) - 1));

      // add token
      strncat(datadog_metric_name, token, (MAX_BUFFER_SIZE - strlen(datadog_metric_name) - strlen(token) - 1));
    }

    // try to generate tokens before we iterate again
    token = strtok_r(NULL, metric_separator, &ctxt);
  }

  // add extra tags from comma-separated string of tags in config.tags
  if (config.tags != NULL) {
    struct uwsgi_string_list *usl = config.tags;
    while (usl) {
      char *tag = usl->value;

      // start with tag_separator if we already have some tags, otherwise put the tag_prefix
      if (strlen(datadog_tags)) {
        strncat(datadog_tags, tag_separator, (MAX_BUFFER_SIZE - strlen(datadog_tags) - strlen(tag_separator) - 1));
      } else {
        strncat(datadog_tags, tag_prefix, (MAX_BUFFER_SIZE - strlen(datadog_tags) - strlen(tag_prefix) - 1));
      }

      // append new tag
      strncat(datadog_tags, tag, (MAX_BUFFER_SIZE - strlen(datadog_tags) - strlen(tag) - 1));

      usl = usl->next;
    }
  }

  return strlen(datadog_metric_name);
}


static int dogstatsd_send_metric(struct uwsgi_buffer *ub, struct uwsgi_stats_pusher_instance *uspi, char *metric, size_t metric_len, int64_t value, char type[2]) {
  struct dogstatsd_node *sn = (struct dogstatsd_node *) uspi->data;

  char datadog_metric_name[MAX_BUFFER_SIZE];
  char datadog_tags[MAX_BUFFER_SIZE];
  char raw_metric_name[MAX_BUFFER_SIZE];

  int extracted_tags = 0;

  // check if we can handle such a metric length
  if (metric_len >= MAX_BUFFER_SIZE)
    return -1;

  // reset the buffer
  ub->pos = 0;

  // sanitize buffers
  memset(datadog_tags, 0, MAX_BUFFER_SIZE);
  memset(datadog_metric_name, 0, MAX_BUFFER_SIZE);

  // let's copy original metric name before we start
  strncpy(raw_metric_name, metric, metric_len + 1);

  // try to extract tags
  extracted_tags = dogstatsd_generate_tags(raw_metric_name, metric_len, datadog_metric_name, datadog_tags);

  if (extracted_tags < 0)
    return -1;

  if (uwsgi_buffer_append(ub, sn->prefix, sn->prefix_len)) return -1;
  if (uwsgi_buffer_append(ub, ".", 1)) return -1;

  // put the datadog_metric_name if we found some tags
  if (extracted_tags) {
    if (uwsgi_buffer_append(ub, datadog_metric_name, strlen(datadog_metric_name))) return -1;
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

  if (sendto(sn->fd, ub->buf, ub->pos, 0, (struct sockaddr *) &sn->addr.sa_in, sn->addr_len) < 0) {
    uwsgi_error("dogstatsd_send_metric()/sendto()");
  }

  return 0;
}


static void stats_pusher_dogstatsd(struct uwsgi_stats_pusher_instance *uspi, time_t now, char *json, size_t json_len) {

  if (!uspi->configured) {
    struct dogstatsd_node *sn = uwsgi_calloc(sizeof(struct dogstatsd_node));
    char *comma = strchr(uspi->arg, ',');
    if (comma) {
      sn->prefix = comma+1;
      sn->prefix_len = strlen(sn->prefix);
      *comma = 0;
    }
    else {
      sn->prefix = "uwsgi";
      sn->prefix_len = 5;
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
        .options = dogstatsd_options,
        .on_load = dogstatsd_init,
};
