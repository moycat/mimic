#include <argp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "args.h"
#include "bpf/filter.h"
#include "bpf/skel.h"
#include "log.h"
#include "util.h"

static volatile sig_atomic_t exiting = 0;
static void sig_int(int signo) { exiting = 1; }

int main(int argc, char* argv[]) {
  int result, retcode = 0;
  struct arguments args = {0};
  try_msg(argp_parse(&args_argp, argc, argv, 0, 0, &args), "error parsing arguments");

  struct mimic_filter filters[args.filter_count];
  for (int i = 0; i < args.filter_count; i++) {
    struct mimic_filter* filter = &filters[i];
    memset(filter, 0, sizeof(*filter));
    char* filter_str = args.filters[i];
    char* delim_pos = strchr(filter_str, '=');
    if (delim_pos == NULL || delim_pos == filter_str)
      ret_with_error(1, "filter format should look like `{key}={value}`: %s", filter_str);

    if (strncmp("local=", args.filters[i], 6) == 0) {
      filter->direction = DIR_LOCAL;
    } else if (strncmp("remote=", args.filters[i], 7) == 0) {
      filter->direction = DIR_REMOTE;
    } else {
      *delim_pos = '\0';
      ret_with_error(2, "unsupported filter type `%s`", filter_str);
    }

    char* value = delim_pos + 1;
    char* port_str = strrchr(value, ':');
    if (!port_str) ret_with_error(3, "no port number specified: %s", value);
    *port_str = '\0';
    port_str++;
    char* endptr;
    long port = strtol(port_str, &endptr, 10);
    if (port <= 0 || port > 65535 || *endptr != '\0')
      ret_with_error(4, "invalid port number: `%s`", port_str);
    filter->port = htons((__u16)port);

    int af;
    if (strchr(value, ':')) {
      if (*value != '[' || port_str[-2] != ']')
        ret_with_error(5, "did you forget square brackets around an IPv6 address?");
      filter->protocol = TYPE_IPV6;
      value++;
      port_str[-2] = '\0';
      af = AF_INET6;
    } else {
      filter->protocol = TYPE_IPV4;
      af = AF_INET;
    }
    if (inet_pton(af, value, &filter->ip.v6) == 0) ret_with_error(1, "bad IP address: %s", value);
    char fmt[FILTER_FMT_MAX_LEN];
    mimic_filter_fmt(filter, fmt);
    printf("%s\n", fmt);
  }

  int ifindex;
  if (!args.ifname) ret_with_error(1, "no interface specified");
  ifindex = if_nametoindex(args.ifname);
  if (!ifindex) ret_with_error(1, "no interface named `%s`", args.ifname);

  libbpf_set_print(libbpf_print_fn);

  struct mimic_bpf* skel =
    try_ptr_msg(mimic_bpf__open_and_load(), "failed to open and load BPF program: %s", strerrno);

  _Bool value = 1;
  for (int i = 0; i < args.filter_count; i++) {
    result = bpf_map__update_elem(
      skel->maps.mimic_whitelist, &filters[i], sizeof(struct mimic_filter), &value, sizeof(_Bool),
      BPF_ANY
    );
    if (result) {
      char fmt[FILTER_FMT_MAX_LEN];
      mimic_filter_fmt(&filters[i], fmt);
      cleanup_with_error(-result, "failed to add filter `%s`: %s", fmt, strerrno);
    }
  }

  LIBBPF_OPTS(bpf_tc_hook, tc_hook_egress, .ifindex = ifindex, .attach_point = BPF_TC_EGRESS);
  LIBBPF_OPTS(bpf_tc_opts, tc_opts_egress, .handle = 1, .priority = 1);

  result = bpf_tc_hook_create(&tc_hook_egress);
  if (result && result != -EEXIST)
    cleanup_with_error(-result, "failed to create TC egress hook: %s", strerrno);

  tc_opts_egress.prog_fd = bpf_program__fd(skel->progs.egress_handler);
  try_cleanup_msg(
    bpf_tc_attach(&tc_hook_egress, &tc_opts_egress), "failed to attach to TC egress hook: %s",
    strerrno
  );

  LIBBPF_OPTS(bpf_tc_hook, tc_hook_ingress, .ifindex = ifindex, .attach_point = BPF_TC_INGRESS);
  LIBBPF_OPTS(bpf_tc_opts, tc_opts_ingress, .handle = 1, .priority = 1);

  result = bpf_tc_hook_create(&tc_hook_ingress);
  if (result && result != -EEXIST)
    cleanup_with_error(-result, "failed to create TC ingress hook: %s", strerrno);

  tc_opts_ingress.prog_fd = bpf_program__fd(skel->progs.ingress_handler);
  try_cleanup_msg(
    bpf_tc_attach(&tc_hook_ingress, &tc_opts_ingress), "failed to attach to TC ingress hook: %s",
    strerrno
  );

  if (signal(SIGINT, sig_int) == SIG_ERR)
    cleanup_with_error(errno, "cannot set signal handler: %s", strerrno);
  while (!exiting) sleep(1);

cleanup:
  printf("cleanup\n");
  tc_opts_egress.flags = tc_opts_egress.prog_fd = tc_opts_egress.prog_id = 0;
  bpf_tc_detach(&tc_hook_egress, &tc_opts_egress);
  // bpf_tc_hook_destroy(&tc_hook_egress);

  tc_opts_ingress.flags = tc_opts_ingress.prog_fd = tc_opts_ingress.prog_id = 0;
  bpf_tc_detach(&tc_hook_ingress, &tc_opts_ingress);
  // bpf_tc_hook_destroy(&tc_hook_ingress);

  mimic_bpf__destroy(skel);
  return retcode;
}
