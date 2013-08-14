/*
 * Handler for HTTP requests.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_ADDRESS_STICKY_H
#define BENG_PROXY_ADDRESS_STICKY_H

#include <inline/compiler.h>

struct sockaddr;

#ifdef __cplusplus
extern "C" {
#endif

gcc_pure
unsigned
socket_address_sticky(const struct sockaddr *address);

#ifdef __cplusplus
}
#endif

#endif
