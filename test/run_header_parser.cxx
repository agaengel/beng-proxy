#include "RootPool.hxx"
#include "header_parser.hxx"
#include "growing_buffer.hxx"
#include "strmap.hxx"

#include <unistd.h>
#include <stdio.h>

int main(int argc gcc_unused, char **argv gcc_unused) {
    char buffer[16];
    ssize_t nbytes;
    struct strmap *headers;

    RootPool pool;

    GrowingBuffer *gb = growing_buffer_new(pool, sizeof(buffer));

    /* read input from stdin */

    while ((nbytes = read(0, buffer, sizeof(buffer))) > 0)
        gb->Write(buffer, (size_t)nbytes);

    /* parse the headers */

    headers = strmap_new(pool);
    header_parse_buffer(pool, headers, gb);

    /* dump headers */

    for (const auto &i : *headers)
        printf("%s: %s\n", i.key, i.value);
}
