#include "cache.hxx"
#include "pool.hxx"
#include "event/Event.hxx"

#include <assert.h>
#include <time.h>

static void *
match_to_ptr(int match)
{
    return (void*)(long)match;
}

static int
ptr_to_match(void *p)
{
    return (int)(long)p;
}

struct my_cache_item {
    struct cache_item item;

    struct pool *pool;
    int match;
    int value;
};

static bool
my_cache_validate(struct cache_item *item)
{
    struct my_cache_item *i = (struct my_cache_item *)item;

    (void)i;
    return true;
}

static void
my_cache_destroy(struct cache_item *item)
{
    struct my_cache_item *i = (struct my_cache_item *)item;
    struct pool *pool = i->pool;

    p_free(pool, i);
    pool_unref(pool);
}

static const struct cache_class my_cache_class = {
    .validate = my_cache_validate,
    .destroy = my_cache_destroy,
};

static struct my_cache_item *
my_cache_item_new(struct pool *pool, int match, int value)
{
    pool = pool_new_linear(pool, "my_cache_item", 1024);
    auto i = NewFromPool<struct my_cache_item>(*pool);
    cache_item_init_relative(&i->item, 3600, 1);
    i->pool = pool;
    i->match = match;
    i->value = value;

    return i;
}

static bool
my_match(const struct cache_item *item, void *ctx)
{
    const struct my_cache_item *i = (const struct my_cache_item *)item;
    int match = ptr_to_match(ctx);

    return i->match == match;
}

int main(int argc gcc_unused, char **argv gcc_unused) {
    struct pool *pool;
    struct cache *cache;
    struct my_cache_item *i;

    EventBase event_base;

    pool = pool_new_libc(nullptr, "root");

    cache = cache_new(*pool, &my_cache_class, 1024, 4);

    /* add first item */

    i = my_cache_item_new(pool, 1, 0);
    cache_put(cache, "foo", &i->item);

    /* overwrite first item */

    i = my_cache_item_new(pool, 2, 0);
    cache_put(cache, "foo", &i->item);

    /* check overwrite result */

    i = (struct my_cache_item *)cache_get(cache, "foo");
    assert(i != nullptr);
    assert(i->match == 2);
    assert(i->value == 0);

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(1));
    assert(i == nullptr);

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(2));
    assert(i != nullptr);
    assert(i->match == 2);
    assert(i->value == 0);

    /* add new item */

    i = my_cache_item_new(pool, 1, 1);
    cache_put_match(cache, "foo", &i->item, my_match, match_to_ptr(1));

    /* check second item */

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(1));
    assert(i != nullptr);
    assert(i->match == 1);
    assert(i->value == 1);

    /* check first item */

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(2));
    assert(i != nullptr);
    assert(i->match == 2);
    assert(i->value == 0);

    /* overwrite first item */

    i = my_cache_item_new(pool, 1, 3);
    cache_put_match(cache, "foo", &i->item, my_match, match_to_ptr(1));

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(1));
    assert(i != nullptr);
    assert(i->match == 1);
    assert(i->value == 3);

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(2));
    assert(i != nullptr);
    assert(i->match == 2);
    assert(i->value == 0);

    /* overwrite second item */

    i = my_cache_item_new(pool, 2, 4);
    cache_put_match(cache, "foo", &i->item, my_match, match_to_ptr(2));

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(1));
    assert(i != nullptr);
    assert(i->match == 1);
    assert(i->value == 3);

    i = (struct my_cache_item *)cache_get_match(cache, "foo",
                                                my_match, match_to_ptr(2));
    assert(i != nullptr);
    assert(i->match == 2);
    assert(i->value == 4);

    /* cleanup */

    cache_close(cache);

    pool_unref(pool);
    pool_commit();
    pool_recycler_clear();
}
