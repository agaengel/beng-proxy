/*
 * The address of a resource stored on a HTTP (or AJP) server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_HTTP_ADDRESS_HXX
#define BENG_PROXY_HTTP_ADDRESS_HXX

#include "address_list.hxx"
#include "glibfwd.hxx"

#include <inline/compiler.h>

#include <stddef.h>

struct pool;
struct StringView;
class MatchInfo;
class Error;

struct HttpAddress {
    enum class Protocol : uint8_t {
        HTTP,
        AJP,
    } protocol;

    bool ssl;

    /**
     * The host part of the URI (including the port, if any).  nullptr if
     * this is HTTP over UNIX domain socket.
     */
    const char *host_and_port;

    /**
     * The path component of the URI, starting with a slash.
     */
    const char *path;

    /**
     * The value of #TRANSLATE_EXPAND_PATH.  Only used by the
     * translation cache.
     */
    const char *expand_path;

    AddressList addresses;

    HttpAddress(Protocol _protocol, bool _ssl,
                const char *_host_and_port, const char *_path);

    HttpAddress(ShallowCopy, Protocol _protocol, bool _ssl,
                const char *_host_and_port, const char *_path,
                const AddressList &_addresses);

    constexpr HttpAddress(ShallowCopy shallow_copy, const HttpAddress &src)
        :protocol(src.protocol), ssl(src.ssl),
         host_and_port(src.host_and_port),
         path(src.path),
         expand_path(src.expand_path),
         addresses(shallow_copy, src.addresses)
    {
    }

    HttpAddress(struct pool &pool, const HttpAddress &src);
    HttpAddress(struct pool &pool, const HttpAddress &src, const char *_path);

    /**
     * Check if this instance is relative to the base, and return the
     * relative part.  Returns nullptr if both URIs do not match.
     */
    gcc_pure
    StringView RelativeTo(const HttpAddress &base) const;

    /**
     * Build the absolute URI from this object, but use the specified path
     * instead.
     */
    gcc_malloc
    char *GetAbsoluteURI(struct pool *pool, const char *override_path) const;

    /**
     * Build the absolute URI from this object.
     */
    gcc_malloc
    char *GetAbsoluteURI(struct pool *pool) const;

    gcc_pure
    bool HasQueryString() const;

    /**
     * Duplicates this #http_address object and inserts the specified
     * query string into the URI.
     */
    gcc_malloc
    HttpAddress *InsertQueryString(struct pool &pool,
                                   const char *query_string) const;

    /**
     * Duplicates this #http_address object and inserts the specified
     * arguments into the URI.
     */
    gcc_malloc
    HttpAddress *InsertArgs(struct pool &pool,
                            StringView args, StringView path_info) const;

    gcc_pure
    bool IsValidBase() const;

    gcc_malloc
    HttpAddress *SaveBase(struct pool *pool, const char *suffix) const;

    gcc_malloc
    HttpAddress *LoadBase(struct pool *pool, const char *suffix) const;

    const HttpAddress *Apply(struct pool *pool, StringView relative) const;

    /**
     * Does this address need to be expanded with http_address_expand()?
     */
    gcc_pure
    bool IsExpandable() const {
        return expand_path != nullptr;
    }

    bool Expand(struct pool *pool, const MatchInfo &match_info,
                Error &error_r);

    gcc_pure
    int GetDefaultPort() const {
        switch (protocol) {
        case Protocol::HTTP:
            return ssl ? 443 : 80;

        case Protocol::AJP:
            return 8009;
        }

        gcc_unreachable();
    }
};

/**
 * Parse the given absolute URI into a newly allocated
 * #http_address object.
 *
 * @return nullptr on error
 */
gcc_malloc
HttpAddress *
http_address_parse(struct pool *pool, const char *uri, GError **error_r);

/**
 * Create a new #http_address object from the specified one, but
 * replace the "path" attribute.  The string pointers are stored,
 * they are not duplicated.
 */
gcc_malloc
HttpAddress *
http_address_with_path(struct pool &pool,
                       const HttpAddress *uwa,
                       const char *path);

gcc_malloc
HttpAddress *
http_address_dup(struct pool &pool, const HttpAddress *uwa);

/**
 * Create a new #http_address object from the specified one, but
 * replace the "path" attribute.  The strings from the source object
 * are duplicated, but the "path" parameter is not.
 */
gcc_malloc
HttpAddress *
http_address_dup_with_path(struct pool &pool,
                           const HttpAddress *uwa,
                           const char *path);

#endif
