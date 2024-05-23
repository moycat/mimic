#include <argp.h>
#include <bpf/bpf.h>
#include <linux/bpf.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>

#include "../common/defs.h"
#include "../common/try.h"
#include "log.h"
#include "mimic.h"

static const struct argp_option options[] = {
  {"process", 'p', NULL, 0, N_("Show process information")},
  {"connections", 'c', NULL, 0, N_("Show connections")},
  {},
};

static inline error_t args_parse_opt(int key, char* arg, struct argp_state* state) {
  struct show_args* args = (typeof(args))state->input;
  switch (key) {
    case 'p':
      args->show_process = true;
      break;
    case 'c':
      args->show_command = true;
      break;
    case ARGP_KEY_ARG:
      if (!args->ifname) {
        args->ifname = arg;
      } else {
        return ARGP_ERR_UNKNOWN;
      }
      break;
    case ARGP_KEY_NO_ARGS:
      argp_usage(state);
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

const struct argp show_argp = {
  options,
  args_parse_opt,
  N_("<interface>"),
  N_("\vSee mimic(1) for detailed usage."),
};

int subcmd_show(struct show_args* args) {
  int ifindex = if_nametoindex(args->ifname);
  if (!ifindex) ret(-1, _("no interface named '%s'"), args->ifname);

  char lock[64];
  struct lock_content c;
  get_lock_file_name(lock, sizeof(lock), ifindex);
  {
    _cleanup_file FILE* lock_file =
      try_p(fopen(lock, "r"), _("failed to open lock file at %s: %s"), lock, strret);
    try(parse_lock_file(lock_file, &c));
  }

  if (!args->show_process && !args->show_command) {
    args->show_process = args->show_command = true;
  }

  if (args->show_process) {
    printf(_("%sMimic%s running on %s\n"), BOLD GREEN, RESET, args->ifname);
    printf(_("  %spid:%s %d\n"), BOLD, RESET, c.pid);
    printf(_("  %ssettings:%s handshake %d:%d, keepalive %d:%d:%d:%d\n"), BOLD, RESET, c.settings.hi,
           c.settings.hr, c.settings.kt, c.settings.ki, c.settings.kr, c.settings.ks);

    _cleanup_fd int whitelist_fd =
      try(bpf_map_get_fd_by_id(c.whitelist_id), _("failed to get fd of map '%s': %s"),
          "mimic_whitelist", strret);

    char buf[FILTER_FMT_MAX_LEN];
    struct filter filter;
    struct filter_settings settings;
    struct bpf_map_iter iter = {.map_fd = whitelist_fd, .map_name = "mimic_whitelist"};

    while (try(bpf_map_iter_next(&iter, &filter))) {
      filter_fmt(&filter, buf);
      try(bpf_map_lookup_elem(whitelist_fd, &filter, &settings),
          _("failed to get value from map '%s': %s"), "mimic_whitelist", strret);
      printf(_("  %sfilter:%s %s"), BOLD, RESET, buf);
      if (filter_settings_eq(&settings, &c.settings)) {
        printf("\n");
      } else {
        printf(_(" %s(handshake %d:%d, keepalive %d:%d:%d:%d)%s\n"), GRAY, settings.hi, settings.hr,
               settings.kt, settings.ki, settings.kr, settings.ks, RESET);
      }
    }
    if (!iter.has_key) printf(_("  %sno active filter%s\n"), BOLD, RESET);
    printf("\n");
  }

  if (args->show_command) {
    _cleanup_fd int conns_fd = try(bpf_map_get_fd_by_id(c.conns_id),
                                   _("failed to get fd of map '%s': %s"), "mimic_conns", strret);

    char local[IP_PORT_MAX_LEN], remote[IP_PORT_MAX_LEN];
    struct conn_tuple key;
    struct connection conn;
    struct bpf_map_iter iter = {.map_fd = conns_fd, .map_name = "mimic_conns"};

    while (try(bpf_map_iter_next(&iter, &key))) {
      ip_port_fmt(key.protocol, key.local, key.local_port, local);
      ip_port_fmt(key.protocol, key.remote, key.remote_port, remote);
      printf(_("%sConnection%s %s => %s\n"), BOLD GREEN, RESET, local, remote);

      try(bpf_map_lookup_elem_flags(conns_fd, &key, &conn, BPF_F_LOCK),
          _("failed to get value from map '%s': %s"), "mimic_conns", strret);

      printf(_("  %sstate:%s %s\n"), BOLD, RESET, gettext(conn_state_to_str(conn.state)));
      printf(_("  %ssequence:%s seq=%08x, ack=%08x\n"), BOLD, RESET, conn.seq, conn.ack_seq);
    }
    if (!iter.has_key) printf(_("%sConnection%s no active connection\n"), BOLD YELLOW, RESET);
    printf("\n");
  }

  return 0;
}
