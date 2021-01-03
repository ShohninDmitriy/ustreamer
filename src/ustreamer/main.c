/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPG-HTTP streamer.                    #
#                                                                            #
#    Copyright (C) 2018  Maxim Devaev <mdevaev@gmail.com>                    #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#include <assert.h>
#ifdef NDEBUG
#	error WTF dude? Asserts are good things!
#endif

#include <limits.h>
#if CHAR_BIT != 8
#	error There are not 8 bits in a char!
#endif

#include <stdio.h>
#include <stdbool.h>
#include <signal.h>

#include <pthread.h>
#ifdef WITH_OMX
#	include <bcm_host.h>
#	include <IL/OMX_Core.h>
#endif

#include "../libs/common/tools.h"
#include "../libs/common/threading.h"
#include "../libs/common/logging.h"

#include "options.h"
#include "device.h"
#include "encoder.h"
#include "stream.h"
#include "http/server.h"
#ifdef WITH_GPIO
#	include "gpio/gpio.h"
#endif


typedef struct {
	stream_s *stream;
	server_s *server;
} _main_context_s;

static _main_context_s *_ctx;

static void _block_thread_signals(void) {
	sigset_t mask;
	assert(!sigemptyset(&mask));
	assert(!sigaddset(&mask, SIGINT));
	assert(!sigaddset(&mask, SIGTERM));
	assert(!pthread_sigmask(SIG_BLOCK, &mask, NULL));
}

static void *_stream_loop_thread(UNUSED void *arg) {
	A_THREAD_RENAME("stream");
	_block_thread_signals();
	stream_loop(_ctx->stream);
	return NULL;
}

static void *_server_loop_thread(UNUSED void *arg) {
	A_THREAD_RENAME("http");
	_block_thread_signals();
	server_loop(_ctx->server);
	return NULL;
}

static void _signal_handler(int signum) {
	switch (signum) {
		case SIGTERM:	LOG_INFO_NOLOCK("===== Stopping by SIGTERM ====="); break;
		case SIGINT:	LOG_INFO_NOLOCK("===== Stopping by SIGINT ====="); break;
		default:		LOG_INFO_NOLOCK("===== Stopping by %d =====", signum); break;
	}
	stream_loop_break(_ctx->stream);
	server_loop_break(_ctx->server);
}

static void _install_signal_handlers(void) {
	struct sigaction sig_act;
	MEMSET_ZERO(sig_act);

	assert(!sigemptyset(&sig_act.sa_mask));
	sig_act.sa_handler = _signal_handler;
	assert(!sigaddset(&sig_act.sa_mask, SIGINT));
	assert(!sigaddset(&sig_act.sa_mask, SIGTERM));

	LOG_DEBUG("Installing SIGINT handler ...");
	assert(!sigaction(SIGINT, &sig_act, NULL));

	LOG_DEBUG("Installing SIGTERM handler ...");
	assert(!sigaction(SIGTERM, &sig_act, NULL));

	LOG_DEBUG("Ignoring SIGPIPE ...");
	assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
}

int main(int argc, char *argv[]) {
	assert(argc >= 0);

	LOGGING_INIT;
	A_THREAD_RENAME("main");

	options_s *options = options_init(argc, argv);
	device_s *dev = device_init();
	encoder_s *encoder = encoder_init();
	stream_s *stream = stream_init(dev, encoder);
	server_s *server = server_init(stream);

	int exit_code = 0;

	if ((exit_code = options_parse(options, dev, encoder, stream, server)) == 0) {
#		ifdef WITH_OMX
		bool i_bcm_host = false;
		if (encoder->type == ENCODER_TYPE_OMX) {
			LOG_INFO("Initializing BCM Host ...");
			bcm_host_init();
			i_bcm_host = true;
		}
		bool i_omx_core = false
		if (encodeer->type == ENCODER_TYPE_OMX) {
			LOG_INFO("Initializing OMX Core ...");
			if ((OMX_ERRORTYPE error = OMX_Init()) != OMX_ErrorNone) {
				LOG_ERROR_OMX(error, "Can't initialize OMX Core");
				exit_code = -1;
				goto omx_error;
			}
			i_omx_core = true;
		}
#		endif

#		ifdef WITH_GPIO
		gpio_init();
#		endif

		_install_signal_handlers();

		_main_context_s ctx;
		ctx.stream = stream;
		ctx.server = server;
		_ctx = &ctx;

		if ((exit_code = server_listen(server)) == 0) {
#			ifdef WITH_GPIO
			gpio_set_prog_running(true);
#			endif

			pthread_t stream_loop_tid;
			pthread_t server_loop_tid;
			A_THREAD_CREATE(&stream_loop_tid, _stream_loop_thread, NULL);
			A_THREAD_CREATE(&server_loop_tid, _server_loop_thread, NULL);
			A_THREAD_JOIN(server_loop_tid);
			A_THREAD_JOIN(stream_loop_tid);
		}

#		ifdef WITH_GPIO
		gpio_set_prog_running(false);
		gpio_destroy();
#		endif

#		ifdef WITH_OMX
		omx_error:
		if (i_omx_core) {
			OMX_Deinit();
		}
		if (i_bcm_host) {
			bcm_host_deinit();
		}
#		endif
	}

	server_destroy(server);
	stream_destroy(stream);
	encoder_destroy(encoder);
	device_destroy(dev);

	options_destroy(options);
	if (exit_code == 0) {
		LOG_INFO("Bye-bye");
	}
	LOGGING_DESTROY;
	return (exit_code < 0 ? 1 : 0);
}