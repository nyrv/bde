// bsls_log.cpp                                                       -*-C++-*-
#include <bsls_log.h>

#include <bsls_ident.h>
BSLS_IDENT("$Id$ $CSID$")

#include <bsls_assert.h>
#include <bsls_asserttest.h>       // for testing only
#include <bsls_atomicoperations.h> // Atomic pointers
#include <bsls_bsltestutil.h>      // for testing only
#include <bsls_platform.h>         // 'BSLS_PLATFORM_OS_WINDOWS'

#include <stdarg.h> // 'va_list', 'va_start', 'va_end', 'va_copy'
#include <stdio.h>  // 'puts', 'snprintf', 'vsnprintf'
#include <stdlib.h> // 'malloc', 'free'
#include <string.h> // 'strlen'

#ifdef BSLS_PLATFORM_OS_WINDOWS
#include <windows.h> // 'GetStdHandle'
#endif

#if defined(BSLS_PLATFORM_CMP_MSVC) && !defined(va_copy)
#define va_copy(dest, src) (dest = src)
// Because VS doesn't define 'va_copy' until VS2013, we must define it
// ourselves, knowing how variadic arguments are implemented on Windows.  In
// both the x86 and x64 cases, 'va_list' is implemented as a simple pointer,
// meaning that 'va_copy' can simply be replaced with an assignment.
#endif

namespace BloombergLP {
namespace bsls {
namespace {
                         // =======================
                         // class BufferScopedGuard
                         // =======================
class BufferScopedGuard {
    // This class implements a scoped guard that unconditionally deallocates
    // the memory allocated by its 'allocate' method.

  private:
    // DATA
    char *d_buffer_p;                // pointer to the allocated buffer (owned)

    // NOT IMPLEMENTED
    BufferScopedGuard(const BufferScopedGuard&);                    // = delete

    BufferScopedGuard& operator=(const BufferScopedGuard&);         // = delete

  public:
    // CREATORS
    BufferScopedGuard();
        // Create a scoped guard that does not own an initial buffer.

    ~BufferScopedGuard();
        // Deallocate the last buffer allocated by a call to the 'allocate'
        // method, and destroy this object.

    // MANIPULATORS
    char* allocate(size_t numBytes);
        // Return a pointer to a buffer guaranteed to be large enough to store
        // the specified 'numBytes' bytes, or 'NULL' if memory was not
        // available.  Deallocate the last buffer allocated by a call to this
        // method.  The returned pointer is owned by this object and will
        // remain valid until this object is destroyed or until this method is
        // called again.

};

// CREATORS
BufferScopedGuard::BufferScopedGuard()
: d_buffer_p(NULL)
{
}

BufferScopedGuard::~BufferScopedGuard()
{
    if(d_buffer_p) {
        free(d_buffer_p);
    }
}

// MANIPULATORS
char* BufferScopedGuard::allocate(size_t numBytes) {

    // Note that we don't want to use realloc, because it guarantees that the
    // contents of the buffer are copied to the new block (in the case that the
    // block is moved).  The code itself does not rely on this ability, so we
    // do not want the inefficiency of the copy if it does not need to be done.

    if(d_buffer_p) {
        free(d_buffer_p);
    }

    d_buffer_p = static_cast<char*>(malloc(numBytes));

    return d_buffer_p;
}

// ============================================================================
//                      LOCAL FUNCTION DEFINITIONS
// ============================================================================

int vsnprintf_alwaysCount(char       *buffer,
                          size_t      size,
                          const char *format,
                          va_list     substitutions)
    // Load, into the specified 'buffer' having the specified 'size', the
    // null-terminated output string formed by applying the 'printf'-style
    // formatting rules to the specified 'format' string using the specified
    // 'substitutions' when 'buffer' is large enough to hold this output
    // string; otherwise, the contents of the buffer will be unspecified.
    // Return the length of the formatted output string (not including the
    // terminating null character) regardless of 'size'.  The behavior is
    // undefined unless 'buffer' contains sufficient space to store 'size'
    // bytes and 'format' is a valid 'printf'-style format string with all
    // expected substitutions present in 'substitutions'.
{

    // Unfortunately, 'vsnprintf' has observably different behavior on
    // different systems.  From the documentation for the various systems, we
    // have the following table of behaviors:
    //..
    //  =======================================================================
    //               'vsnprintf' behavior in case of overflow
    //  -----------------------------------------------------------------------
    //  System                   Return Value                   Null Written?
    //  ---------   --------------------------------------    -----------------
    //  Microsoft   # Written Chars (not incl. '\0') or -1    No
    //  BSD         # Expected Chars (not incl. '\0')         Yes
    //  GCC         # Expected Chars (not incl. '\0')         No
    //  Solaris     # Expected Chars (not incl. '\0')         No
    //  AIX         # Expected Chars (not incl. '\0')         No
    //  =======================================================================
    //..
    // On Microsoft, we simply use a different function to generate the count.
    // To solve the BSD / other inconsistency, we simply specify in our
    // contract that the output string is undefined unless there is enough room
    // to hold the additional zero byte.
    BSLS_ASSERT_OPT(buffer);
    BSLS_ASSERT(format);

    int count;

#ifdef BSLS_PLATFORM_OS_WINDOWS
    // Because we are making *two* calls using 'substitutions', we must make a
    // copy so the first call does not ruin the 'va_list'.
    va_list substitutions_copy;
    va_copy(substitutions_copy, substitutions);
    count = _vscprintf(format, substitutions_copy);
    va_end(substitutions_copy);

    if(count >= 0) {
        // Because 'count >= 0', the cast to 'size_t' is safe.
        const size_t countCasted = static_cast<size_t>(count);
        if(size > countCasted) {
            count = vsnprintf(buffer, size, format, substitutions);
        }
    }
#else
    count = vsnprintf(buffer, size, format, substitutions);
#endif

    return count;
}

int vsnprintf_allocate(char                 *originalBuffer,
                       size_t                originalBufferSize,
                       BufferScopedGuard&    guard,
                       char                **outputBuffer,
                       size_t               *outputBufferSize,
                       const char           *format,
                       va_list               substitutions)
    // Load into the specified 'outputBuffer' the address of a buffer
    // containing the null-terminated string formed by applying the
    // 'printf'-style formatting rules to the specified 'format' string using
    // the specified 'substitutions'.  Load into the specified
    // 'outputBufferSize' a size that is at least the number of bytes in the
    // formatted string, including the terminating null byte.  If the specified
    // 'originalBuffer', having the specified 'originalBufferSize', is large
    // enough to hold the entire formatted string, then 'originalBuffer' will
    // be the loaded output buffer.  Otherwise, 'outputBuffer' will point to a
    // buffer allocated using the specified 'guard', and the contents of
    // 'originalBuffer' will be unspecified.  Return the number of characters
    // in the formatted string, not including the terminating null byte.  If a
    // memory allocation was necessary but available was insufficient, return a
    // negative value; in this case, values stored for the output buffer and
    // its size are unspecified.  The behavior is undefined unless
    // 'originalBuffer' contains at least 'originalBufferSize' bytes, and
    // 'format' is a valid 'printf'-style format string with all expected
    // substitutions present in 'substitutions'.
{
    BSLS_ASSERT_OPT(originalBuffer);
    BSLS_ASSERT_OPT(outputBuffer);
    BSLS_ASSERT_OPT(outputBufferSize);
    BSLS_ASSERT_OPT(format);

    size_t  bufferSize = originalBufferSize;
    char   *buffer     = originalBuffer;

    // Because we are making *two* calls using 'substitutions', we must make a
    // copy so the first call does not ruin the 'va_list'.
    va_list substitutions_copy;
    va_copy(substitutions_copy, substitutions);
    int status = vsnprintf_alwaysCount(buffer,
                                       bufferSize,
                                       format,
                                       substitutions_copy);
    va_end(substitutions_copy);

    if(status >= 0) {
        // Cast is safe because status is nonnegative
        const size_t statusCasted = static_cast<size_t>(status);
        if(statusCasted + 1 > originalBufferSize) {
            // The number of needed characters did not fit in the buffer, so we
            // must allocate and then call 'vsnprintf' again (this time, we do
            // not need the alwaysCount variant).
            bufferSize = statusCasted + 1;
            buffer = guard.allocate(bufferSize);

            if(buffer) {
                const int newStatus = vsnprintf(buffer,
                                                bufferSize,
                                                format,
                                                substitutions);
                if(newStatus != status) {
                    // Some weird error.
                    if(newStatus < 0) {
                        // If the new status was negative then we should return
                        // the new status so the user can get better error
                        // information:
                        status = newStatus;
                    } else {
                        // Otherwise, if it was non-negative but not the
                        // expected value, we should let the user know that we
                        // failed, so we will set the status to a negative
                        // value.  This would only happen due to undefined
                        // behavior, so we can really return anything:
                        status = -2;
                    }
                }
            } else {
                // Allocation error.  Just set the status to a negative value:
                status = -1;
            }
        }
    }

    *outputBufferSize = bufferSize;
    *outputBuffer = buffer;
    return status;
}

int snprintf_allocate(char                 *originalBuffer,
                      size_t                originalBufferSize,
                      BufferScopedGuard&    guard,
                      char                **outputBuffer,
                      size_t               *outputBufferSize,
                      const char           *format,
                      ...)
    // Load into the specified 'outputBuffer' the address of a buffer
    // containing the null-terminated string formed by applying the
    // 'printf'-style formatting rules to the specified 'format' string using
    // the specified '...' as substitutions.  Load into the specified
    // 'outputBufferSize' a size that is at least the number of bytes in the
    // formatted string, including the terminating null byte.  If the specified
    // 'originalBuffer', having the specified 'originalBufferSize', is large
    // enough to hold the entire formatted string, then 'originalBuffer' will
    // be the loaded output buffer.  Otherwise, 'outputBuffer' will point to a
    // buffer allocated using the specified 'guard', and the contents of
    // 'originalBuffer' will be unspecified.  Return the number of characters
    // in the formatted string, not including the terminating null byte.  If a
    // memory allocation was necessary but the available memory was
    // insufficient, return a negative value; in this case, values stored for
    // the output buffer and its size are unspecified.  The behavior is
    // undefined unless 'originalBuffer' contains at least 'originalBufferSize'
    // bytes, and 'format' is a valid 'printf'-style format string with all
    // expected substitutions present in '...'.
{
    va_list substitutions;
    va_start(substitutions, format);
    const int status = vsnprintf_allocate(originalBuffer,
                                          originalBufferSize,
                                          guard,
                                          outputBuffer,
                                          outputBufferSize,
                                          format,
                                          substitutions);
    va_end(substitutions);
    return status;
}

}  // close unnamed namespace

                         // =========
                         // class Log
                         // =========

// CLASS DATA
bsls::AtomicOperations::AtomicTypes::Pointer Log::s_logMessageHandler =
    {reinterpret_cast<void*>(&Log::platformDefaultMessageHandler)};
    // Static initialization of the Log::s_logMessageHandler function pointer
    // which holds the log handler function.  The pointer is initialized to the
    // address of the 'static' function 'platformDefaultMessageHandler'.

// CLASS METHODS
void Log::logFormattedMessage(const char *file,
                              int         line,
                              const char *format,
                              ...)
{

    // Scoped guard to handle the buffer if it needs to be dynamically
    // allocated.
    BufferScopedGuard guard;

    // This is the initial stack-allocated buffer which we will use to store
    // the formatted string if it can fit.
    const size_t originalBufferSize = 1024;
    char         originalBuffer[originalBufferSize];

    // This is the "final buffer" we will use when outputting our value to the
    // logging function.  We will let 'vsnprintf_allocate' set this value.
    size_t  bufferSize;
    char   *buffer;

    va_list substitutions;
    va_start(substitutions, format);
    const int status = vsnprintf_allocate(originalBuffer,
                                          originalBufferSize,
                                          guard,
                                          &buffer,
                                          &bufferSize,
                                          format,
                                          substitutions);
    va_end(substitutions);

    if(status < 0) {
        BSLS_LOG_SIMPLE("Low-level log failure.");
        // Weird error.  Could be a memory allocation failure.  Just quit.
        return;                                                       // RETURN
    }

    bsls::Log::logMessage(file, line, buffer);
}

void Log::platformDefaultMessageHandler(const char *file,
                                        const int   line,
                                        const char *message)
{

#ifdef BSLS_PLATFORM_OS_WINDOWS
    // First, we will check if we have a valid handle to 'stderr'.
    const HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
    if(   stderrHandle == NULL
       || stderrHandle == INVALID_HANDLE_VALUE
       || GetConsoleWindow() == NULL) {
        // Even if the handle is neither null nor 'INVALID_HANDLE_VALUE', it
        // may still cause an error to write to it, because a console could
        // have been available and someone could have called 'FreeConsole',
        // which closes the handles but does not set them to NULL.  Therefore,
        // to be safe, we also check for 'GetConsoleWindow'.  Checking both
        // cases ensures the safest behavior.

        BSLS_ASSERT_OPT(file);
        BSLS_ASSERT(line >= 0);
        BSLS_ASSERT_OPT(message);
        // To avoid the multi-threaded interlacing of messages, we need to
        // pre-format a buffer and then pass that to 'OutputDebugStringA'.

        // This is the initial stack-allocated buffer which we will use to
        // store the formatted string if it can fit:
        const size_t originalBufferSize = 1024;
        char originalBuffer[originalBufferSize];

        // This is the "final buffer" we will use when outputting our value to
        // the logging function.  We will let 'snprintf_allocate' set this
        // value:
        size_t  bufferSize;
        char   *buffer;

        BufferScopedGuard guard;

        const int status = snprintf_allocate(originalBuffer,
                                             originalBufferSize,
                                             guard,
                                             &buffer,
                                             &bufferSize,
                                             "%s:%d %s\n",
                                             file,
                                             line,
                                             message);

        if(status >= 4) {
            // Ensure no weird errors happened.  At least four characters must
            // have been written.  Note that checking for 'status >= 4' and not
            // 'status >= 0' is simply a convenience, and should not make a
            // difference in practice, unless one of the functions is somehow
            // exhibiting undefined behavior due to bad user input.

            OutputDebugStringA(buffer);
        } else {
            // There are legitimate reasons for 'status' to be negative, such
            // as a memory allocation failure.  Therefore, we will log a simple
            // error so that we don't fail quietly:

            OutputDebugStringA("Low-level log failure.\n");
        }
    } else {
        stderrMessageHandler(file, line, message);
    }
#else
    // In non-Windows, we will just use 'stderr'.
    stderrMessageHandler(file, line, message);
#endif

}

void Log::stderrMessageHandler(const char *file,
                               int         line,
                               const char *message)
{
    BSLS_ASSERT_OPT(file);
    BSLS_ASSERT(line >= 0);
    BSLS_ASSERT_OPT(message);

    fprintf(stderr, "%s:%d %s\n", file, line, message);
    fflush(stderr);
}

void Log::stdoutMessageHandler(const char *file,
                               int         line,
                               const char *message)
{
    BSLS_ASSERT_OPT(file);
    BSLS_ASSERT(line >= 0);
    BSLS_ASSERT_OPT(message);

    fprintf(stdout, "%s:%d %s\n", file, line, message);
    fflush(stdout);
}

}  // close package namespace
}  // close enterprise namespace

// ----------------------------------------------------------------------------
// Copyright (C) 2014 Bloomberg Finance L.P.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------- END-OF-FILE ----------------------------------