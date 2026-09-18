#ifndef FTPVITA_RETR_H
#define FTPVITA_RETR_H

#include <limits.h>
#include <stdint.h>

typedef int (*ftpvita_retr_read_fn)(void *, void *, unsigned int);
typedef int (*ftpvita_retr_send_fn)(void *, const void *, unsigned int, int *);

typedef struct {
	const char *stage;
	int code;
	int net_errno;
	uint64_t bytes_read;
	uint64_t bytes_sent;
} ftpvita_retr_result_t;

/* Transfer only [start, extent), with the descriptor already at start. Taking
 * extent once from the opened file makes an append-only log a finite RETR.
 * This is an extent bound, not a snapshot of concurrently overwritten data.
 * A short EOF is an error, not a successful/truncated evidence download.
 * Callbacks use the sceIoRead/sceNetSend byte-count/error contracts. */
static inline ftpvita_retr_result_t ftpvita_retr_extent(void *context,
		ftpvita_retr_read_fn read_file, ftpvita_retr_send_fn send_data,
		void *buffer, unsigned int buffer_size, uint64_t start, uint64_t extent)
{
	ftpvita_retr_result_t result = {"complete", 0, 0, 0, 0};
	uint64_t remaining;
	if (!read_file || !send_data || !buffer || buffer_size == 0 ||
		buffer_size > INT_MAX || start > extent) {
		result.stage = "range";
		return result;
	}
	remaining = extent - start;
	while (remaining > 0) {
		const unsigned int requested = remaining < buffer_size ?
			(unsigned int)remaining : buffer_size;
		const int bytes_read = read_file(context, buffer, requested);
		unsigned int position = 0;
		if (bytes_read <= 0 || (unsigned int)bytes_read > requested) {
			result.stage = bytes_read == 0 ? "early-eof" : "read";
			result.code = bytes_read;
			return result;
		}
		result.bytes_read += (unsigned int)bytes_read;
		remaining -= (unsigned int)bytes_read;
		while (position < (unsigned int)bytes_read) {
			const unsigned int pending = (unsigned int)bytes_read - position;
			int net_error = 0;
			const int sent = send_data(context,
				(const unsigned char *)buffer + position, pending, &net_error);
			if (sent <= 0 || (unsigned int)sent > pending) {
				result.stage = "send";
				result.code = sent;
				result.net_errno = sent < 0 ? net_error : 0;
				return result;
			}
			position += (unsigned int)sent;
			result.bytes_sent += (unsigned int)sent;
		}
	}
	return result;
}

#endif
