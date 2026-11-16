#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *ErrorCodeToString(ErrorCode e)
{
  switch (e) {
    case kErrorNone: return "ErrorNone";
    case kErrorOutOfMem: return "ErrorOutOfMem";
    case kErrorInvalidParam: return "ErrorInvalidParam";
    case kErrorOverflow: return "ErrorOverflow";
    case kErrorUndeflow: return "ErrorUndeflow";
    case kErrorParsing: return "ErrorParsing";
    case kErrorNotFound: return "ErrorNotFound";
    case kErrorOS: return "ErrorOS";
#ifndef CORE_NO_PLATFORM_DEPS
    case kErrorProcessSpawnFail: return "ErrorProcessSpawnFail";
    case kErrorProcessWaitFail: return "ErrorProcessWaitFail";
    case kErrorProcessTermFail: return "ErrorProcessTermFail"; 
#endif // CORE_NO_PLATFORM_DEPS
  }
  return "Unknown error";
}

//============================================================================//
// Allocator implementation: The library simply provides a small wrapper over //
// the libc functions.                                                        //
// The allocator interface is mostly meant to serve as a spec for the user to //
// define new allocators depending on the behaviour they want for their new   //
// projects.                                                                  //
//============================================================================//

static ErrorCode LibcAlloc(void **ptr, size_t bytes)
{
  *ptr = malloc(bytes);
  if (!(*ptr)) { return kErrorOutOfMem; }
  return kErrorNone;
}

static ErrorCode LibcRealloc(void **ptr, size_t realloc_size, size_t old_size)
{
  void *new_block = malloc(realloc_size);
  if (!new_block) { return kErrorOutOfMem; }
  memmove(new_block, *ptr, old_size < realloc_size ? old_size : realloc_size);
  free(*ptr);
  *ptr = new_block;
  return kErrorNone;
}

static ErrorCode LibcFree(void *ptr)
{
  free(ptr);
  return kErrorNone;
}

ErrorCode LibcAllocatorProc(void *ctx, AllocatorRequest *req)
{
  UNUSED(ctx);
  switch (req->operation) {
    case kAllocatorOpAlloc: return LibcAlloc(req->ptr, req->new_sz);
    case kAllocatorOpRealloc: return LibcRealloc(req->ptr, req->new_sz,
                                  req->old_sz);
    case kAllocatorOpFree: return LibcFree(req->ptr);
    default: break;
  }
// If we got here it means that the user provided a bad request type, so we
  // just report the error
  return kErrorInvalidParam; 
}

Allocator GetLibcAllocator(void)
{
  Allocator libc_alloc = { 0 };
  libc_alloc.proc = LibcAllocatorProc;
  return libc_alloc;
}

//============================================================================//
// Char handling implementation                                               //
//============================================================================//

bool CharIsSpace(char c)
{
  return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
}

//============================================================================//
// String view implementation                                                 //
//============================================================================//

Str StrTrim(Str s)
{
  return StrTrimLeft(StrTrimRight(s));
}

Str StrTrimLeft(Str s)
{
  while (s.length > 0 && CharIsSpace(s.data[0])) { s.data++; s.length--; }
  return s;
}

Str StrTrimRight(Str s)
{
  while (s.length > 0 && CharIsSpace(s.data[s.length - 1])) { s.length--; }
  return s;
}

Str StrChopLeft(Str *s, char delimiter)
{
  Str ret = *s;
  while (s->length > 0 && s->data[0] != delimiter) { s->data++; s->length--; }
  ret.length -= s->length;
  if (s->length > 0) { s->data++; s->length--; }
  return ret;
}

size_t StrCountChar(Str s, char c)
{
  size_t count = 0;
  for (size_t i = 0; i < s.length; i++) { count += (s.data[i] == c); }
  return count;
}

Str StrFromParts(const char *data, size_t len)
{
  Str s;
  s.data = data;
  s.length = len;
  return s;
}

ErrorCode LoadFileAsStr(Allocator *alloc, const char *path, Str *out)
{
  if (!path || !alloc || !out) { return kErrorInvalidParam; }
  void *raw;
  ErrorCode ret_code = kErrorNone;
  FILE *f = fopen(path, "rb");
  if (!f) { return kErrorNotFound; }
  if (fseek(f, 0, SEEK_END) != 0) { ret_code = kErrorOS; goto end_func; } 
  long bytes = ftell(f);
  if (bytes < 0) { ret_code = kErrorOS; goto end_func; } 
  if (fseek(f, 0, SEEK_SET) != 0) { ret_code = kErrorOS; goto end_func; } 
  if ((ret_code = ALLOCATE(alloc, &raw, (size_t)bytes, _Alignof(void*)))
      != kErrorNone) {
    goto end_func;
  }
  if (fread(raw, 1, bytes, f) != (size_t)bytes) {
    DEALLOCATE(alloc, raw, bytes);
    ret_code = kErrorOS;
    goto end_func;
  }
  out->data = (const char *)raw;
  out->length = bytes;
end_func:
  fclose(f);
  return ret_code;
}

#ifndef CORE_NO_PLATFORM_DEPS

#ifdef _WIN32
  #include <windows.h>
#else
  // For now, when we are not running on windows, we assume that we are running
  // on an OS with Posix compatibility
  #include <fcntl.h>
  #include <spawn.h>
  #include <errno.h>
  #include <unistd.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <sys/types.h>
  extern char **environ;
#endif 

ErrorCode StartCommand(const char *command)
{
  if (!command || command[0] == '\0') { return kErrorInvalidParam; }
#ifdef _WIN32
  STARTUPINFOA startup = { 0 };
  PROCESS_INFORMATION process = { 0 };
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  SECURITY_ATTRIBUTES sa = { 0 };
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, NULL);
  if (nul == INVALID_HANDLE_VALUE) { return kErrorProcessSpawnFail; }
  startup.hStdInput = nul;
  startup.hStdOutput = nul;
  startup.hStdError = nul;
  char cmdline[4096] = { 0 };
  int len = snprintf(cmdline, sizeof(cmdline), "cmd.exe /D /S /C \"%s\"",
      command);
  if (len < 0 || (size_t)len >= sizeof(cmdline)) {
    CloseHandle(nul);
    return kErrorOverflow;
  }
  BOOL success = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &startup,
      &process);
  CloseHandle(nul);
  if (!success) { return kErrorProcessSpawnFail; }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return kErrorNone;
#else
  pid_t pid = fork();
  if (pid < 0) { return kErrorProcessSpawnFail; }
  if (pid == 0) {
    if (setsid() < 0) { _exit(1); }
    pid_t child = fork();
    if (child < 0) { _exit(1); }
    if (child > 0) { _exit(0); }
    int nul = open("/dev/null", O_RDWR);
    if (nul < 0) { _exit(1); }
    for (int i = 0; i < 3; i++) { if (dup2(nul, i) < 0) { _exit(1); } }
    if (nul > 2) { close(nul); }
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  int status;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) { continue; }
    return kErrorOS;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return kErrorProcessSpawnFail;
  }
  return kErrorNone;
#endif
}

typedef struct {
#ifdef _WIN32
  HANDLE hProcess;
  HANDLE hThread;
#else
  pid_t pid;
#endif
  int is_running;
} ProcessInternal;

// We have to check that the pubic type has enough space to store the internal
// representation on every OS. We have some extra space for ABI compatibility
// so we can add more fields if needed
STATIC_ASSERT(sizeof(Process) >= sizeof(ProcessInternal), CheckProcessSize);

ErrorCode ProcessStart(const ProcessConfig *pconf, Process *out)
{
  if (!pconf || !pconf->executable_path || !out) { return kErrorInvalidParam; }
  if (!pconf->args || !pconf->args[0]) { return kErrorInvalidParam; }
  memset(out, 0, sizeof(Process));
  ProcessInternal *proc = (ProcessInternal *)out->reserved;
#ifdef _WIN32
  char cmd[4096] = { 0 };
  size_t offset = 0;
  if (pconf->args) {
    for (int i = 0; pconf->args[i] != NULL; i++) {
      if (offset + 2 >= sizeof(cmd)) { return kErrorOverflow; }
      cmd[offset++] = ' ';
      int written = snprintf(cmd + offset, sizeof(cmd) - offset, "\"%s\"",
          pconf->args[i]);
      if (written < 0 || (size_t)written >= sizeof(cmd) - offset) {
        return kErrorOverflow;
      }
      offset += written;
    }
  }

  STARTUPINFOA startup = { 0 };
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;

  SECURITY_ATTRIBUTES sa = { 0 };
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE nul = INVALID_HANDLE_VALUE;
  if (pconf->stdin_stream == kProcessStreamNull
      || pconf->stdout_stream == kProcessStreamNull
      || pconf->stderr_stream == kProcessStreamNull) {
    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (nul == INVALID_HANDLE_VALUE) { return kErrorProcessSpawnFail; }
  }

  startup.hStdInput = (pconf->stdin_stream == kProcessStreamNull)
    ? nul
    : GetStdHandle(STD_INPUT_HANDLE);

  startup.hStdOutput = (pconf->stdout_stream == kProcessStreamNull)
    ? nul
    : GetStdHandle(STD_OUTPUT_HANDLE);

  startup.hStdError  = (pconf->stderr_stream == kProcessStreamNull)
    ? nul
    : GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION process_info = {0};
  BOOL success = CreateProcessA(pconf->executable_path, cmd, NULL, NULL,
      TRUE, 0, NULL, NULL, &startup, &process_info);

  if (nul != INVALID_HANDLE_VALUE) { CloseHandle(nul); }
  if (!success) { return kErrorProcessSpawnFail; }

  proc->hProcess = process_info.hProcess;
  proc->hThread = process_info.hThread;
  proc->is_running = 1;
  return kErrorNone;
#else
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    return kErrorProcessSpawnFail;
  }
  if (pconf->stdin_stream == kProcessStreamNull) {
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
        O_RDONLY, 0) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return kErrorProcessSpawnFail;
    }
  }
  if (pconf->stdout_stream == kProcessStreamNull) {
    if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
        O_WRONLY, 0) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return kErrorProcessSpawnFail;
    }
  }
  if (pconf->stderr_stream == kProcessStreamNull) {
    if (posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
        O_WRONLY, 0) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return kErrorProcessSpawnFail;
    }
  }
  pid_t pid = 0;
  int spawn_results = posix_spawn(&pid, pconf->executable_path, &actions, NULL,
      (char *const *)pconf->args, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_results != 0) { return kErrorProcessSpawnFail; }
  proc->pid = pid;
  proc->is_running = 1;
  return kErrorNone;
#endif
}

ErrorCode ProcessWait(Process *process, int *out_exit_code)
{
  if (!process) { return kErrorInvalidParam; }
  ProcessInternal *proc = (ProcessInternal *)process->reserved;
  if (!proc->is_running) { return kErrorInvalidParam; }
#ifdef _WIN32
  DWORD wait_result = WaitForSingleObject(proc->hProcess, INFINITE);
  if (wait_result != WAIT_OBJECT_0) { return kErrorProcessWaitFail; }
  DWORD exit_code = 0;
  if (GetExitCodeProcess(proc->hProcess, &exit_code)) {
    if (out_exit_code) { *out_exit_code = (int)exit_code; }
  } else {
    return kErrorProcessWaitFail;
  }
  proc->is_running = 0;
  return kErrorNone;
#else
  int status = 0;
  while (waitpid((pid_t)proc->pid, &status, 0) < 0) {
    if (errno == EINTR) { continue; }
    return kErrorProcessWaitFail;
  }
  proc->is_running = 0;
  if (out_exit_code) {
    if (WIFEXITED(status)) {
      *out_exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      *out_exit_code = -WTERMSIG(status);
    } else {
      *out_exit_code = -1;
    }
  }
  return kErrorNone;
#endif
}

ErrorCode ProcessKill(Process *process)
{
  if (!process) { return kErrorInvalidParam; }
  ProcessInternal *proc = (ProcessInternal *)process->reserved;
  if (!proc->is_running) { return kErrorInvalidParam; }
#ifdef _WIN32
  if (!TerminateProcess(proc->hProcess, 1)) { return kErrorProcessTermFail; }
#else
  if (kill((pid_t)proc->pid, SIGTERM) < 0) { return kErrorProcessTermFail; }
#endif
  return kErrorNone;
}

ErrorCode ProcessClose(Process *process)
{
  if (!process) { return kErrorInvalidParam; }
  ProcessInternal *proc = (ProcessInternal *)process->reserved;
  if (proc->is_running) { return kErrorInvalidParam; }
#ifdef _WIN32
  if (proc->hProcess) {
    CloseHandle(proc->hProcess);
    proc->hProcess = NULL;
  }
  if (proc->hThread) {
    CloseHandle(proc->hThread);
    proc->hThread = NULL;
  }
#else
  proc->pid = 0;
#endif
  proc->is_running = 0;
  return kErrorNone;
}

#endif // CORE_NO_PLATFORM_DEPS
