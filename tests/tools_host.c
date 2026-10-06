#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int lua_runtime_run(const char *path, char *error, size_t size) {
  (void)path; (void)error; (void)size;
  return 0;
}

int main(void) {
  char root[] = "/tmp/nintencode-test-XXXXXX";
  assert(mkdtemp(root));
  assert(chdir(root) == 0);
  assert(mkdir("sdmc:", 0700) == 0);
  char output[4096];
  assert(tools_init(output, sizeof(output)));
  FILE *file = fopen("sdmc:/nintencode/workdir/example.txt", "w");
  assert(file);
  fputs("needle in the file\n", file);
  fclose(file);
  assert(mkdir("sdmc:/nintencode/workdir/empty", 0700) == 0);
  assert(tool_execute("grep_files", "{\"pattern\":\"needle\"}", output, sizeof(output)));
  assert(strstr(output, "needle"));
  assert(tool_execute("delete_file", "{\"path\":\"example.txt\"}", output, sizeof(output)));
  assert(access("sdmc:/nintencode/workdir/example.txt", F_OK) != 0);
  assert(tool_execute("delete_file", "{\"path\":\"empty\"}", output, sizeof(output)));
  assert(!tool_execute("delete_file", "{\"path\":\"missing\"}", output, sizeof(output)));
  assert(!tool_execute("delete_file", "{\"path\":\"../outside\"}", output, sizeof(output)));
  remove("sdmc:/nintencode/debug.log");
  assert(rmdir("sdmc:/nintencode/workdir") == 0);
  assert(rmdir("sdmc:/nintencode") == 0);
  assert(rmdir("sdmc:") == 0);
  assert(chdir("/") == 0);
  assert(rmdir(root) == 0);
  puts("Filesystem search, deletion, and path boundary tests passed.");
  return 0;
}
