// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "was/Client.hxx"
#include "was/Launch.hxx"
#include "was/Lease.hxx"
#include "was/MetricsHandler.hxx"
#include "stopwatch.hxx"
#include "lease.hxx"
#include "http/ResponseHandler.hxx"
#include "strmap.hxx"
#include "istream/sink_fd.hxx"
#include "istream/UnusedPtr.hxx"
#include "istream/OpenFileIstream.hxx"
#include "memory/fb_pool.hxx"
#include "PInstance.hxx"
#include "spawn/Config.hxx"
#include "spawn/ChildOptions.hxx"
#include "spawn/Registry.hxx"
#include "spawn/Local.hxx"
#include "io/Logger.hxx"
#include "io/SpliceSupport.hxx"
#include "http/HeaderName.hxx"
#include "http/Method.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Cancellable.hxx"
#include "util/PrintException.hxx"
#include "util/StaticVector.hxx"
#include "util/StringAPI.hxx"
#include "util/StringSplit.hxx"
#include "util/StringStrip.hxx"
#include "AllocatorPtr.hxx"

#include <fmt/core.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

struct Context final
	: PInstance, WasLease, WasMetricsHandler, HttpResponseHandler, SinkFdHandler {

	WasProcess process;

	SinkFd *body = nullptr;
	bool error;

	CancellablePointer cancel_ptr;

	Context():body(nullptr) {}

	/* virtual methods from class WasMetricsHandler */
	void OnWasMetric(std::string_view name, float value) noexcept override {
		fmt::print(stderr, "metric '{}'={}\n", name, value);
	}

	/* virtual methods from class Lease */
	void ReleaseWas([[maybe_unused]] bool reuse) override {
		process.handle.reset();
		process.Close();
	}

	void ReleaseWasStop([[maybe_unused]] uint64_t input_received) override {
		ReleaseWas(false);
	}

	/* virtual methods from class HttpResponseHandler */
	void OnHttpResponse(HttpStatus status, StringMap &&headers,
			    UnusedIstreamPtr body) noexcept override;
	void OnHttpError(std::exception_ptr ep) noexcept override;

	/* virtual methods from class SinkFdHandler */
	void OnInputEof() noexcept override;
	void OnInputError(std::exception_ptr ep) noexcept override;
	bool OnSendError(int error) noexcept override;
};

/*
 * SinkFdHandler
 *
 */

void
Context::OnInputEof() noexcept
{
	body = nullptr;
}

void
Context::OnInputError(std::exception_ptr ep) noexcept
{
	PrintException(ep);

	body = nullptr;
	error = true;
}

bool
Context::OnSendError(int _error) noexcept
{
	fmt::print(stderr, "{}\n", strerror(_error));

	body = nullptr;
	error = true;

	return true;
}

/*
 * http_response_handler
 *
 */

void
Context::OnHttpResponse(HttpStatus status,
			[[maybe_unused]] StringMap &&headers,
			UnusedIstreamPtr _body) noexcept
{
	fmt::print(stderr, "status: {}\n", http_status_to_string(status));

	if (_body) {
		struct pool &pool = root_pool;
		body = sink_fd_new(event_loop, pool,
				   std::move(_body),
				   FileDescriptor(STDOUT_FILENO),
				   guess_fd_type(STDOUT_FILENO),
				   *this);
		sink_fd_read(body);
	}
}

void
Context::OnHttpError(std::exception_ptr ep) noexcept
{
	PrintException(ep);

	error = true;
}

static auto
request_body(EventLoop &event_loop, struct pool &pool)
{
	struct stat st;
	return fstat(0, &st) == 0 && S_ISREG(st.st_mode)
		? OpenFileIstream(event_loop, pool, "/dev/stdin")
		: nullptr;
}

int
main(int argc, char **argv)
try {
	SetLogLevel(5);

	if (argc < 3) {
		fmt::print(stderr, "Usage: run_was PATH URI [--parameter a=b ...]\n");
		return EXIT_FAILURE;
	}

	Context context;

	const char *uri = argv[2];
	StaticVector<const char *, 64> params;

	StringMap headers;

	for (int i = 3; i < argc;) {
		if (StringIsEqual(argv[i], "--parameter") ||
		    StringIsEqual(argv[i], "-p")) {
			++i;
			if (i >= argc)
				throw std::runtime_error("Parameter value missing");

			if (params.full())
				throw std::runtime_error("Too many parameters");

			params.push_back(argv[i++]);
		} else if (StringIsEqual(argv[i], "--header") ||
			   StringIsEqual(argv[i], "-H")) {
			++i;
			if (i >= argc)
				throw std::runtime_error("Header value missing");

			auto [name, value] = Split(std::string_view{argv[i++]}, ':');
			name = Strip(name);
			if (!http_header_name_valid(name) || value.data() == nullptr)
				throw std::runtime_error("Malformed header");

			value = StripLeft(value);

			AllocatorPtr alloc(context.root_pool);
			headers.Add(alloc, alloc.DupToLower(name), alloc.DupZ(value));
		} else
			throw std::runtime_error("Unrecognized parameter");
	}

	direct_global_init();

	SpawnConfig spawn_config;

	const ScopeFbPoolInit fb_pool_init;

	ChildOptions child_options;
	child_options.no_new_privs = true;

	ChildProcessRegistry child_process_registry;
	LocalSpawnService spawn_service(spawn_config, context.event_loop,
					child_process_registry);

	context.process = was_launch(spawn_service, "was",
				     argv[1], {},
				     child_options, {});

	was_client_request(context.root_pool, context.event_loop, nullptr,
			   context.process.control,
			   context.process.input,
			   context.process.output,
			   context,
			   nullptr,
			   HttpMethod::GET, uri,
			   nullptr,
			   nullptr, nullptr,
			   headers,
			   request_body(context.event_loop, context.root_pool),
			   params,
			   &context,
			   context, context.cancel_ptr);

	context.event_loop.Run();

	return context.error;
} catch (const std::exception &e) {
	PrintException(e);
	return EXIT_FAILURE;
}
