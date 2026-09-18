#include "../ftpvita_retr.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	uint64_t position;
	uint64_t available;
	uint64_t expected_position;
	uint64_t received;
	unsigned int read_limit;
	unsigned int send_limit;
	unsigned int read_calls;
	unsigned int send_calls;
	unsigned int fail_read_call;
	unsigned int fail_send_call;
	int read_failure;
	int send_failure;
	int net_error;
	int growing;
} fixture_t;

static int read_file(void *context, void *buffer, unsigned int length)
{
	fixture_t *f = context;
	unsigned char *bytes = buffer;
	assert(++f->read_calls < 100);
	if (f->read_calls == f->fail_read_call)
		return f->read_failure;
	if (f->growing)
		f->available += length;
	if (f->read_limit && length > f->read_limit)
		length = f->read_limit;
	if (length > f->available - f->position)
		length = (unsigned int)(f->available - f->position);
	for (unsigned int i = 0; i < length; i++)
		bytes[i] = (unsigned char)(f->position + i);
	f->position += length;
	return (int)length;
}

static int send_data(void *context, const void *buffer, unsigned int length,
	int *net_error)
{
	fixture_t *f = context;
	const unsigned char *bytes = buffer;
	assert(++f->send_calls < 100);
	*net_error = f->net_error;
	if (f->send_calls == f->fail_send_call)
		return f->send_failure;
	if (f->send_limit && length > f->send_limit)
		length = f->send_limit;
	for (unsigned int i = 0; i < length; i++)
		assert(bytes[i] == (unsigned char)(f->expected_position + f->received + i));
	f->received += length;
	return (int)length;
}

static ftpvita_retr_result_t transfer(fixture_t *f, uint64_t start, uint64_t end)
{
	unsigned char buffer[8];
	f->position = start;
	f->expected_position = start;
	return ftpvita_retr_extent(f, read_file, send_data, buffer,
		sizeof(buffer), start, end);
}

static void complete(fixture_t *f, uint64_t start, uint64_t end)
{
	const ftpvita_retr_result_t r = transfer(f, start, end);
	assert(strcmp(r.stage, "complete") == 0);
	assert(r.code == 0 && r.net_errno == 0);
	assert(r.bytes_read == end - start && r.bytes_sent == end - start);
	assert(f->position == end && f->received == end - start);
}

int main(void)
{
	fixture_t f = {.available = 19};
	complete(&f, 0, 19); /* multiple blocks and a short final block */
	f = (fixture_t){.available = 19, .read_limit = 3, .send_limit = 2};
	complete(&f, 0, 19); /* partial reads/sends neither duplicate nor drop bytes */
	f = (fixture_t){.available = 19, .growing = 1, .send_limit = 3};
	complete(&f, 0, 19);
	assert(f.available > 19 && f.read_calls == 3); /* never chase appended bytes */
	f = (fixture_t){.available = 19};
	complete(&f, 7, 19); /* REST uses the bounded suffix */
	f = (fixture_t){.available = 19};
	complete(&f, 19, 19);
	assert(f.read_calls == 0 && f.send_calls == 0);
	f = (fixture_t){0};
	complete(&f, 0, 0);
	assert(f.read_calls == 0 && f.send_calls == 0);
	f = (fixture_t){.available = UINT64_C(0x100000013)};
	complete(&f, UINT64_C(0xfffffff8), UINT64_C(0x100000013));
	f = (fixture_t){.available = 19};
	ftpvita_retr_result_t r = transfer(&f, 20, 19);
	assert(strcmp(r.stage, "range") == 0 && f.read_calls == 0);
	f = (fixture_t){.available = 11}; /* concurrent truncation */
	r = transfer(&f, 0, 19);
	assert(strcmp(r.stage, "early-eof") == 0 && r.code == 0);
	assert(r.bytes_read == 11 && r.bytes_sent == 11 && r.net_errno == 0);
	f = (fixture_t){.available = 19, .fail_read_call = 2, .read_failure = -123};
	r = transfer(&f, 0, 19);
	assert(strcmp(r.stage, "read") == 0 && r.code == -123);
	assert(r.bytes_read == 8 && r.bytes_sent == 8 && r.net_errno == 0);
	f = (fixture_t){.available = 19, .send_limit = 3, .fail_send_call = 2,
		.send_failure = -456, .net_error = 35};
	r = transfer(&f, 0, 19);
	assert(strcmp(r.stage, "send") == 0 && r.code == -456 && r.net_errno == 35);
	assert(r.bytes_read == 8 && r.bytes_sent == 3 && f.read_calls == 1);
	f = (fixture_t){.available = 19, .fail_send_call = 1,
		.send_failure = 0, .net_error = 35};
	r = transfer(&f, 0, 19);
	assert(strcmp(r.stage, "send") == 0 && r.code == 0 && r.net_errno == 0);
	assert(r.bytes_sent == 0 && f.read_calls == 1 && f.send_calls == 1);
	f = (fixture_t){.available = 19, .fail_read_call = 1, .read_failure = 9};
	r = transfer(&f, 0, 19);
	assert(strcmp(r.stage, "read") == 0 && r.bytes_read == 0 && f.send_calls == 0);
	f = (fixture_t){.available = 19, .fail_send_call = 1, .send_failure = 9};
	r = transfer(&f, 0, 19);
	assert(strcmp(r.stage, "send") == 0 && r.bytes_sent == 0);
	unsigned char buffer[8];
	r = ftpvita_retr_extent(&f, read_file, send_data, buffer, 0, 0, 1);
	assert(strcmp(r.stage, "range") == 0);
	r = ftpvita_retr_extent(&f, read_file, send_data, buffer, UINT_MAX, 0, 1);
	assert(strcmp(r.stage, "range") == 0);
	r = ftpvita_retr_extent(&f, read_file, send_data, NULL, 8, 0, 1);
	assert(strcmp(r.stage, "range") == 0);
	puts("ftpvita RETR extent/partial I/O/error accounting: ok");
	return 0;
}
