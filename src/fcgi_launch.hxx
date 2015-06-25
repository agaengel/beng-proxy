/*
 * Launch FastCGI child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_FCGI_LAUNCH_HXX
#define BENG_PROXY_FCGI_LAUNCH_HXX

#include <inline/compiler.h>

struct JailParams;
template<typename T> struct ConstBuffer;

gcc_noreturn
void
fcgi_run(const JailParams *jail,
         const char *executable_path,
         ConstBuffer<const char *> args,
         ConstBuffer<const char *> env);

#endif
