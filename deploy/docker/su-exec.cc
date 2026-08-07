/* Minimal privilege-drop helper for BusyBox images (fully static).
 * Usage: su-exec user[:group] command [args...]
 *
 * BusyBox `su` treats leading dashes as its own options, so it cannot
 * forward flags like `--data-dir` / `--help` to the child process.
 */
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
  char* user_spec;
  char* group_name = NULL;
  char* colon;
  const struct passwd* pw;
  const struct group* gr;
  gid_t gid;

  if (argc < 3) {
    fprintf(stderr, "Usage: su-exec user[:group] command [args...]\n");
    return 2;
  }

  user_spec = argv[1];
  colon = strchr(user_spec, ':');
  if (colon != NULL) {
    *colon = '\0';
    group_name = colon + 1;
  }

  pw = getpwnam(user_spec);
  if (pw == NULL) {
    fprintf(stderr, "su-exec: unknown user: %s\n", user_spec);
    return 1;
  }

  gid = pw->pw_gid;
  if (group_name != NULL && group_name[0] != '\0') {
    gr = getgrnam(group_name);
    if (gr == NULL) {
      fprintf(stderr, "su-exec: unknown group: %s\n", group_name);
      return 1;
    }
    gid = gr->gr_gid;
  }

  if (setgid(gid) != 0) {
    perror("su-exec: setgid");
    return 1;
  }
  if (initgroups(pw->pw_name, gid) != 0) {
    perror("su-exec: initgroups");
    return 1;
  }
  if (setuid(pw->pw_uid) != 0) {
    perror("su-exec: setuid");
    return 1;
  }

  execvp(argv[2], argv + 2);
  perror("su-exec: exec");
  return 1;
}
