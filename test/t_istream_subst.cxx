// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "IstreamFilterTest.hxx"
#include "istream/SubstIstream.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream.hxx"
#include "istream/UnusedPtr.hxx"

class IstreamSubstTestTraits {
public:
	static constexpr const char *expected_result = "xyz bar fo fo bar bla! fo";

	static constexpr bool call_available = true;
	static constexpr bool enable_blocking = true;
	static constexpr bool enable_abort_istream = true;

	UnusedIstreamPtr CreateInput(struct pool &pool) const noexcept {
		return istream_string_new(pool, "xyz foo fo fo bar blablablablubb fo");
	}

	UnusedIstreamPtr CreateTest(EventLoop &, struct pool &pool,
				    UnusedIstreamPtr input) const noexcept {
		SubstTree tree;
		tree.Add(pool, "foo", "bar");
		tree.Add(pool, "blablablubb", "!");

		return UnusedIstreamPtr(istream_subst_new(&pool, std::move(input), std::move(tree)));
	}
};

INSTANTIATE_TYPED_TEST_CASE_P(Subst, IstreamFilterTest,
			      IstreamSubstTestTraits);
