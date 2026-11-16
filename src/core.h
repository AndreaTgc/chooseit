#ifndef CORE_H
#define CORE_H

#include <stddef.h>
#include <stdbool.h>

#define UNUSED(x) ((void)(x))
#define STATIC_ASSERT(cond, m) typedef int StaticAssertCheck##m[(cond) ? 1 : -1]

// Simple type for propagating errors that happen inside functions.
typedef int ErrorCode;

// Here we have a pre-defined set of errors that are meant to be used inside the
// code of this library.
// If the user wishes to add new error codes, they can either extend this enum
// or define a new integer alias that starts after the last error code so that
// they are compatible with each other.
// Some additional error codes are also defined in other sections of this header
// (search CORE_NO_PLATFORM_DEPS for an example)
enum {
  kErrorNone         = 0,
  kErrorOutOfMem     = 1,
  kErrorInvalidParam = 2,
  kErrorOverflow     = 3,
  kErrorUndeflow     = 4,
  kErrorParsing      = 5,
  kErrorNotFound     = 6,
  kErrorOS           = 7
};

// Converts an error code to a string representation. This function also takes
// care of the optional error codes for other sections of this header (as long
// as the related macros are defined)
// Returns "UNKNOWN_ERROR" when the parameter doesn't match any of the enums.
const char *ErrorCodeToString(ErrorCode e);

#define TRY(expr) do {                                                         \
  ErrorCode tmperr = expr;                                                     \
  if (tmperr != kErrorNone) { return tmperr; }                                 \
} while (0)

//============================================================================//
// The Allocator interface defined here is a very small spec for how to write //
// allocator independent code. The idea is that we define a spec for defining //
// an allocator and, as long as the implementation adheres to the spec, the   //
// code that relied on a different allocator implementation (without relying  //
// on implementation specific behaviour) doesn't need to be changed.          //
//============================================================================//

typedef enum {
  kAllocatorOpAlloc   = 0, // Allocates a new block
  kAllocatorOpRealloc = 1, // Reallocates a block (unchanged on failure) 
  kAllocatorOpFree    = 2, // Deallocates a block
} AllocatorOp;

typedef struct {
  // This field decides which operation is going to be run. The procedure that
  // is stored inside the allocator will automatically dispatch to the correct
  // path based on this.
  AllocatorOp operation;

  // This pointer is used by all the memory handling procedures in this way:
  // - Alloc   -> used as out parameter
  // - Realloc -> used as in/out parameter, if the realloc fails the value is
  //              unchanged
  // - Free    -> used as in parameter for the pointer to free
  void *ptr;

  // These parameters define the allocation/reallocation/free parameters, the
  // free procedure also take the size in order to make it usable for types of
  // allocators that do not keep track of sizes internally.
  size_t old_sz;
  size_t new_sz;
  size_t align_of;
  
  // We also take the source location of the caller so that we can use it for
  // debugging and optionally keep track of leaks when using a debug allocator.
  const char *file;
  size_t line;
} AllocatorRequest;

typedef ErrorCode (*AllocatorProc)(void *ctx, AllocatorRequest *req);

// The allocator structure is a thin wrapper over the procedure and parameters,
// you can either use the structure or just call the function directly, based on
// how you want your code to look like
typedef struct {
  // The context pointer is basically the way to access the allocator state for
  // implementations that are stateful.
  void *ctx;
  // the proc pointer just points to the specific procedure that handles all the
  // allocator operation
  AllocatorProc proc;
} Allocator;

Allocator GetLibcAllocator(void);

#define ALLOCATE(alloc, out, size, align)                                      \
  (alloc)->proc((alloc)->ctx, &((AllocatorRequest) { .ptr = out,               \
        .new_sz = size, .align_of = align, .file = __FILE__, .line = __LINE__, \
        .operation = kAllocatorOpAlloc }))

#define REALLOCATE(alloc, p, oldsz, newsz, align)                              \
  (alloc)->proc((alloc)->ctx, &((AllocatorRequest) { .ptr = p,                 \
        .old_sz = oldsz, .new_sz = newsz, .align_of = align,                   \
        .file = __FILE__, .line = __LINE__, .operation = kAllocatorOpRealloc }))

#define DEALLOCATE(alloc, tofree, size)                                        \
  (alloc)->proc((alloc)->ctx, &((AllocatorRequest) { .ptr = tofree,            \
        .old_sz = size, .operation = kAllocatorOpFree }))

bool CharIsSpace(char c);

//============================================================================//
// String view: this type can be used for programs where you have no need to  //
// modify the original string and you just want to trim/tokenize/split views  // 
// inside that string. It's non owning and faster than creating new strings   //
// for every operation                                                        //
//============================================================================//

typedef struct {
  const char *data;
  size_t length;
} Str;

#define STR_FMT_ARGS(s) (int)((s).length), (s).data

Str StrTrim(Str s);
Str StrTrimLeft(Str s);
Str StrTrimRight(Str s);
Str StrChopLeft(Str *s, char delimiter);
Str StrFromParts(const char *data, size_t len);
size_t StrCountChar(Str s, char c);
ErrorCode LoadFileAsStr(Allocator *alloc, const char *path, Str *out);

//============================================================================//
// Platform API wrappers: this section is optional and is used to interact    //
// with the OS we are running on for things like starting tasks and using the //
// other utilities that are provided (shared memories, file system etc).      //
//============================================================================//
#ifndef CORE_NO_PLATFORM_DEPS

// We define additional errors that are used specifically for functions that
// interact with the OS, we start at 100 so that the user has more space for
// defining their own generic errors before they reach these ones.
enum {
  kErrorProcessSpawnFail = 100, // The OS couldn't start a new process
  kErrorProcessWaitFail  = 101, // Failed to wait for process completion
  kErrorProcessTermFail  = 102  // Failed to terminate (or kill) the process
};

// This type is used to decide whether a spawned process should inherit the std
// file handles of the parent, we may want to do this but most of the times i
// find myself pushing all the outputs to the nul so that they can be discarded
typedef enum {
  kProcessStreamInherit = 0, // Inherits the parent streams 
  kProcessStreamNull    = 1, // Redirects to null (/dev/null or NUL)
} ProcessStreamType;

// This type is used as a definition on how a spawned process should behave. The
// user has to set at the very least the executable path so that it can be used
// for calling the application.
// The args array is optional (except for args[0] which has to be the exe name,
// it's a an array where the last element has to be NULL (this is how we know
// the arguments are finished) and they are passed to the process by the OS.
// The user also has to define the config for the std file handles.
typedef struct {
  const char *executable_path;     // Path to the executable we want to run
  const char * const *args;        // Array of arguments (last must be NULL)
  ProcessStreamType stdin_stream;  // Config for stdin
  ProcessStreamType stdout_stream; // Config for stdout
  ProcessStreamType stderr_stream; // Config for stderr
} ProcessConfig;

// This process type is used as an opaque block that can also be allocated on
// the stack or statically by the user.
// You should NEVER access the internal memory of this type as it's a place that
// the library uses to set OS variables that are needed for the different OS we
// support.
typedef struct {
  int reserved[32]; // DO NOT TOUCH THIS (implementation defined)
} Process;

// This function executes a command via the OS shell, it should be used only
// with trusted commands (please)
ErrorCode StartCommand(const char *command);

// Starts a new process using the configuration provided as input. On success we
// also set the value of out so that it can be used with the other process APIs.
ErrorCode ProcessStart(const ProcessConfig *pconf, Process *out);

// Waits for a process to terminate, on success we also set the exit code.
// NOTE: if the user wants to discard the exit code they can just pass NULL.
ErrorCode ProcessWait(Process *process, int *out_exit_code); 

ErrorCode ProcessKill(Process *process);
ErrorCode ProcessClose(Process *process);

#endif // CORE_NO_PLATFORM_DEPS

#endif // CORE_H
