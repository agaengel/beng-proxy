/*
 * SSL/TLS configuration.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "ssl_factory.hxx"
#include "ssl_config.hxx"
#include "ssl_domain.hxx"
#include "SniCallback.hxx"
#include "Error.hxx"
#include "Basic.hxx"
#include "Unique.hxx"
#include "Name.hxx"
#include "AltName.hxx"
#include "Util.hxx"
#include "util/AllocatedString.hxx"
#include "util/StringView.hxx"

#include <inline/compiler.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <algorithm>
#include <forward_list>

#include <assert.h>

struct SslFactoryCertKey {
    struct Name {
        AllocatedString<> value;
        size_t length;

        Name(AllocatedString<> &&_value)
            :value(std::move(_value)), length(strlen(value.c_str())) {}

        Name(const char *_value)
            :value(AllocatedString<>::Duplicate(_value)),
             length(strlen(_value)) {}

        gcc_pure
        bool Match(StringView host_name) const;
    };

    UniqueSSL_CTX ssl_ctx;

    std::forward_list<Name> names;

    SslFactoryCertKey() = default;

    SslFactoryCertKey(SslFactoryCertKey &&other) = default;
    SslFactoryCertKey &operator=(SslFactoryCertKey &&other) = default;

    void LoadServer(const SslConfig &parent_config,
                    const SslCertKeyConfig &config);

    void CacheCommonName(X509_NAME *subject) {
        auto common_name = NidToString(*subject, NID_commonName);
        if (common_name != nullptr)
            names.emplace_front(std::move(common_name));
    }

    void CacheCommonName(X509 *cert) {
        X509_NAME *subject = X509_get_subject_name(cert);
        if (subject != nullptr)
            CacheCommonName(subject);

        for (const auto &i : GetSubjectAltNames(*cert))
            names.emplace_front(i.c_str());
    }

    gcc_pure
    bool MatchCommonName(StringView host_name) const;

    UniqueSSL Make() const {
        UniqueSSL ssl(SSL_new(ssl_ctx.get()));
        if (!ssl)
            throw SslError("SSL_new() failed");

        return ssl;
    }

    void Apply(SSL *ssl) const {
        SSL_set_SSL_CTX(ssl, ssl_ctx.get());
    }

    unsigned Flush(long tm);
};

struct SslFactory {
    std::vector<SslFactoryCertKey> cert_key;

    const std::unique_ptr<SslSniCallback> sni;

    SslFactory(std::unique_ptr<SslSniCallback> &&_sni)
        :sni(std::move(_sni)) {}

    gcc_pure
    const SslFactoryCertKey *FindCommonName(StringView host_name) const;

    void EnableSNI();

    UniqueSSL Make();

    unsigned Flush(long tm);
};

static void
load_certs_keys(SslFactory &factory, const SslConfig &config)
{
    factory.cert_key.reserve(config.cert_key.size());

    for (const auto &c : config.cert_key) {
        SslFactoryCertKey ck;
        ck.LoadServer(config, c);

        factory.cert_key.emplace_back(std::move(ck));
    }
}

static void
ApplyServerConfig(SSL_CTX *ssl_ctx, const SslCertKeyConfig &cert_key)
{
    ERR_clear_error();

    if (SSL_CTX_use_RSAPrivateKey_file(ssl_ctx,
                                       cert_key.key_file.c_str(),
                                       SSL_FILETYPE_PEM) != 1)
        throw SslError("Failed to load key file " +
                       cert_key.key_file);

    if (SSL_CTX_use_certificate_chain_file(ssl_ctx,
                                           cert_key.cert_file.c_str()) != 1)
        throw SslError("Failed to load certificate file " +
                       cert_key.cert_file);
}

inline bool
SslFactoryCertKey::Name::Match(StringView host_name) const
{
    if (value == nullptr)
        return false;

    if (length == host_name.size &&
        memcmp(host_name.data, value.c_str(), host_name.size) == 0)
        return true;

    if (value[0] == '*' && value[1] == '.' && value[2] != 0) {
        if (host_name.size >= length &&
            /* match only one segment (no dots) */
            memchr(host_name.data, '.',
                   host_name.size - length + 1) == nullptr &&
            memcmp(host_name.data + host_name.size - length + 1,
                   value.c_str() + 1, length - 1) == 0)
            return true;
    }

    return false;
}

inline bool
SslFactoryCertKey::MatchCommonName(StringView host_name) const
{
    for (const auto &name : names)
        if (name.Match(host_name))
            return true;

    return false;
}

inline const SslFactoryCertKey *
SslFactory::FindCommonName(StringView host_name) const
{
    for (const auto &ck : cert_key)
        if (ck.MatchCommonName(host_name))
            return &ck;

    return nullptr;
}

static void
PrintException(const std::exception &e)
{
    fprintf(stderr, "%s\n", e.what());
    try {
        std::rethrow_if_nested(e);
    } catch (const std::exception &nested) {
        PrintException(nested);
    } catch (...) {
        fprintf(stderr, "Unrecognized nested exception\n");
    }
}

static int
ssl_servername_callback(SSL *ssl, gcc_unused int *al,
                        const SslFactory &factory)
{
    const char *_host_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (_host_name == nullptr)
        return SSL_TLSEXT_ERR_OK;

    const StringView host_name(_host_name);

    /* find the first certificate that matches */

    const auto *ck = factory.FindCommonName(host_name);
    if (ck != nullptr) {
        /* found it - now use it */
        ck->Apply(ssl);
    } else if (factory.sni) {
        try {
            factory.sni->OnSni(ssl, host_name.data);
        } catch (const std::exception &e) {
            PrintException(e);
        }
    }

    return SSL_TLSEXT_ERR_OK;
}

inline void
SslFactory::EnableSNI()
{
    SSL_CTX *ssl_ctx = cert_key.front().ssl_ctx.get();

    if (!SSL_CTX_set_tlsext_servername_callback(ssl_ctx,
                                                ssl_servername_callback) ||
        !SSL_CTX_set_tlsext_servername_arg(ssl_ctx, this))
        throw SslError("SSL_CTX_set_tlsext_servername_callback() failed");
}

inline UniqueSSL
SslFactory::Make()
{
    auto ssl = cert_key.front().Make();

    SSL_set_accept_state(ssl.get());

    return ssl;
}

inline unsigned
SslFactoryCertKey::Flush(long tm)
{
    unsigned before = SSL_CTX_sess_number(ssl_ctx.get());
    SSL_CTX_flush_sessions(ssl_ctx.get(), tm);
    unsigned after = SSL_CTX_sess_number(ssl_ctx.get());
    return after < before ? before - after : 0;
}

inline unsigned
SslFactory::Flush(long tm)
{
    unsigned n = 0;
    for (auto &i : cert_key)
        n += i.Flush(tm);
    return n;
}

void
SslFactoryCertKey::LoadServer(const SslConfig &parent_config,
                              const SslCertKeyConfig &config)
{
    assert(ssl_ctx == nullptr);

    ssl_ctx = CreateBasicSslCtx(true);

    assert(!parent_config.cert_key.empty());

    ApplyServerConfig(ssl_ctx.get(), config);
    ApplyServerConfig(ssl_ctx.get(), parent_config);

    auto ssl = Make();

    X509 *cert = SSL_get_certificate(ssl.get());
    if (cert == nullptr)
        throw SslError("No certificate in SSL_CTX");

    EVP_PKEY *key = SSL_get_privatekey(ssl.get());
    if (key == nullptr)
        throw SslError("No certificate in SSL_CTX");

    if (!MatchModulus(*cert, *key))
        throw SslError("Key '" + config.key_file +
                       "' does not match certificate '" +
                       config.cert_file + "'");

    CacheCommonName(cert);
}

SslFactory *
ssl_factory_new_server(const SslConfig &config,
                       std::unique_ptr<SslSniCallback> &&sni)
{
    assert(!config.cert_key.empty());

    std::unique_ptr<SslFactory> factory(new SslFactory(std::move(sni)));

    assert(!config.cert_key.empty());

    load_certs_keys(*factory, config);

    if (factory->cert_key.size() > 1 || factory->sni)
        factory->EnableSNI();

    return factory.release();
}

void
ssl_factory_free(SslFactory *factory)
{
    delete factory;
}

UniqueSSL
ssl_factory_make(SslFactory &factory)
{
    return factory.Make();
}

unsigned
ssl_factory_flush(SslFactory &factory, long tm)
{
    return factory.Flush(tm);
}
