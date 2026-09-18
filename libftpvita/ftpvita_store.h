#ifndef FTPVITA_STORE_H
#define FTPVITA_STORE_H

#include <limits.h>
#include <stdint.h>

typedef int (*ftpvita_store_receive_fn)(void *, void *, unsigned int, int *);
typedef int (*ftpvita_store_write_fn)(void *, const void *, unsigned int);

typedef struct {
	const char *stage;
	int code;
	int net_errno;
	uint64_t bytes_received;
	uint64_t bytes_written;
} ftpvita_store_result_t;

/* Receive until the data peer closes normally. Every received byte is written
 * before another receive. Short writes are completed; zero progress, errors,
 * and callback contract violations abort instead of silently losing data. */
static inline ftpvita_store_result_t ftpvita_store_stream(void *context,
		ftpvita_store_receive_fn receive_data,
		ftpvita_store_write_fn write_file, void *buffer,
		unsigned int buffer_size)
{
	ftpvita_store_result_t result = {"complete", 0, 0, 0, 0};

	if (!receive_data || !write_file || !buffer || buffer_size == 0 ||
		buffer_size > INT_MAX) {
		result.stage = "range";
		return result;
	}

	for (;;) {
		int net_error = 0;
		const int received = receive_data(context, buffer, buffer_size,
			&net_error);
		unsigned int position = 0;
		if (received == 0)
			return result;
		if (received < 0 || (unsigned int)received > buffer_size) {
			result.stage = "receive";
			result.code = received;
			result.net_errno = received < 0 ? net_error : 0;
			return result;
		}
		result.bytes_received += (unsigned int)received;

		while (position < (unsigned int)received) {
			const unsigned int pending = (unsigned int)received - position;
			const int written = write_file(context,
				(const unsigned char *)buffer + position, pending);
			if (written <= 0 || (unsigned int)written > pending) {
				result.stage = "write";
				result.code = written;
				return result;
			}
			position += (unsigned int)written;
			result.bytes_written += (unsigned int)written;
		}
	}
}

#endif
