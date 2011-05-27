/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_ADDRESS_ENVELOPE_H
#define BENG_PROXY_ADDRESS_ENVELOPE_H

#include <sys/socket.h>

struct address_envelope {
    socklen_t length;

    struct sockaddr address;
};

#endif
