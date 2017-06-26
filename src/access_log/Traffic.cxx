/*
 * Print the site name and the bytes transferred for each request.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Server.hxx"
#include "Datagram.hxx"

#include <stdio.h>

static void
dump(const AccessLogDatagram *d)
{
    if (d->site != nullptr && d->valid_traffic)
        printf("%s %llu\n", d->site,
               (unsigned long long)(d->traffic_received + d->traffic_sent));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    struct log_server *server = log_server_new(0);
    while (const auto *d = log_server_receive(server))
        dump(d);

    return 0;
}
