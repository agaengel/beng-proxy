/*
 * Concatenate several istreams.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream_cat.hxx"
#include "istream_oo.hxx"
#include "istream_pointer.hxx"

#include <boost/intrusive/slist.hpp>

#include <iterator>

#include <assert.h>
#include <stdarg.h>

struct CatIstream final : public Istream {
    struct Input
        : boost::intrusive::slist_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> {

        CatIstream &cat;
        IstreamPointer istream;

        Input(CatIstream &_cat, struct istream &_istream)
            :cat(_cat),
            istream(_istream, MakeIstreamHandler<Input>::handler, this) {}

        void Read() {
            istream.Read();
        }

        /* handler */

        size_t OnData(const void *data, size_t length) {
            return cat.OnInputData(*this, data, length);
        }

        ssize_t OnDirect(FdType type, int fd, size_t max_length) {
            return cat.OnInputDirect(*this, type, fd, max_length);
        }

        void OnEof() {
            assert(istream.IsDefined());
            istream.Clear();

            cat.OnInputEof(*this);
        }

        void OnError(GError *error) {
            assert(istream.IsDefined());
            istream.Clear();

            cat.OnInputError(*this, error);
        }

        struct Disposer {
            void operator()(Input *input) {
                if (input->istream.IsDefined())
                    input->istream.Close();
            }
        };
    };

    bool reading = false;

    typedef boost::intrusive::slist<Input,
                                    boost::intrusive::constant_time_size<false>> InputList;
    InputList inputs;

    CatIstream(struct pool &p, va_list ap);

    Input &GetCurrent() {
        return inputs.front();
    }

    const Input &GetCurrent() const {
        return inputs.front();
    }

    bool IsCurrent(const Input &input) const {
        return &GetCurrent() == &input;
    }

    bool IsEOF() const {
        return inputs.empty();
    }

    /**
     * Remove all nulled leading inputs.
     *
     * @return false if there are no more inputs
     */
    bool AutoShift() {
        while (true) {
            if (IsEOF())
                return false;

            if (GetCurrent().istream.IsDefined())
                return true;

            inputs.pop_front();
        }
    }

    void CloseAllInputs() {
        inputs.clear_and_dispose(Input::Disposer());
    }

    size_t OnInputData(Input &i, const void *data, size_t length) {
        return IsCurrent(i)
            ? InvokeData(data, length)
            : 0;
    }

    ssize_t OnInputDirect(gcc_unused Input &i, FdType type, int fd,
                          size_t max_length) {
        assert(IsCurrent(i));

        return InvokeDirect(type, fd, max_length);
    }

    void OnInputEof(Input &i) {
        if (!IsCurrent(i))
            return;

        if (!AutoShift()) {
            DestroyEof();
        } else if (!reading) {
            /* only call Input::Read() if this function was not called
               from CatIstream:Read() - in this case,
               istream_cat_read() would provide the loop.  This is
               advantageous because we avoid unnecessary recursing. */
            GetCurrent().Read();
        }
    }

    void OnInputError(gcc_unused Input &i, GError *error) {
        CloseAllInputs();
        DestroyError(error);
    }

    /* virtual methods from class Istream */

    off_t GetAvailable(bool partial) override;
    void Read() override;
    int AsFd() override;
    void Close() override;
};

/*
 * istream implementation
 *
 */

off_t
CatIstream::GetAvailable(bool partial)
{
    off_t available = 0;

    for (const auto &input : inputs) {
        if (!input.istream.IsDefined())
            continue;

        const off_t a = input.istream.GetAvailable(partial);
        if (a != (off_t)-1)
            available += a;
        else if (!partial)
            /* if the caller wants the exact number of bytes, and
               one input cannot provide it, we cannot provide it
               either */
            return (off_t)-1;
    }

    return available;
}

void
CatIstream::Read()
{
    const ScopePoolRef ref(GetPool() TRACE_ARGS);

    reading = true;

    CatIstream::InputList::const_iterator prev;
    do {
        if (!AutoShift()) {
            DestroyEof();
            break;
        }

        GetCurrent().istream.SetDirect(GetHandlerDirect());

        prev = inputs.begin();
        GetCurrent().istream.Read();
    } while (!IsEOF() && inputs.begin() != prev);

    reading = false;
}

int
CatIstream::AsFd()
{
    /* we can safely forward the as_fd() call to our input if it's the
       last one */

    if (std::next(inputs.begin()) != inputs.end())
        /* not on last input */
        return -1;

    auto &i = GetCurrent();
    int fd = i.istream.AsFd();
    if (fd >= 0)
        Destroy();

    return fd;
}

void
CatIstream::Close()
{
    CloseAllInputs();
    Destroy();
}

/*
 * constructor
 *
 */

inline CatIstream::CatIstream(struct pool &p, va_list ap)
    :Istream(p)
{
    auto i = inputs.before_begin();

    struct istream *istream;
    while ((istream = va_arg(ap, struct istream *)) != nullptr) {
        assert(!istream_has_handler(istream));

        auto *input = NewFromPool<Input>(p, *this, *istream);
        i = inputs.insert_after(i, *input);
    }
}

struct istream *
istream_cat_new(struct pool *pool, ...)
{
    va_list ap;
    va_start(ap, pool);
    auto cat = NewFromPool<CatIstream>(*pool, *pool, ap);
    va_end(ap);
    return cat->Cast();
}
