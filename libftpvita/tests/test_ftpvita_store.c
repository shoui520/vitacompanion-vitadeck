#include "../ftpvita_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	const unsigned char *source;
	unsigned int source_size;
	unsigned int source_position;
	unsigned char destination[64];
	unsigned int destination_position;
	unsigned int receive_limit;
	unsigned int write_limit;
	unsigned int receive_calls;
	unsigned int write_calls;
	unsigned int fail_receive_call;
	unsigned int fail_write_call;
	int receive_failure;
	int write_failure;
	int net_error;
} fixture_t;

static int receive_data(void *context, void *buffer, unsigned int length,
	int *net_error)
{
	fixture_t *f = context;
	assert(++f->receive_calls < 100);
	*net_error = f->net_error;
	if (f->receive_calls == f->fail_receive_call)
		return f->receive_failure;
	if (f->receive_limit && length > f->receive_limit)
		length = f->receive_limit;
	if (length > f->source_size - f->source_position)
		length = f->source_size - f->source_position;
	memcpy(buffer, f->source + f->source_position, length);
	f->source_position += length;
	return (int)length;
}

static int write_file(void *context, const void *buffer, unsigned int length)
{
	fixture_t *f = context;
	assert(++f->write_calls < 100);
	if (f->write_calls == f->fail_write_call)
		return f->write_failure;
	if (f->write_limit && length > f->write_limit)
		length = f->write_limit;
	assert(f->destination_position + length <= sizeof(f->destination));
	memcpy(f->destination + f->destination_position, buffer, length);
	f->destination_position += length;
	return (int)length;
}

static ftpvita_store_result_t transfer(fixture_t *f)
{
	unsigned char buffer[8];
	return ftpvita_store_stream(f, receive_data, write_file, buffer,
		sizeof(buffer));
}

int main(void)
{
	static const unsigned char source[] = "partial I/O must preserve every byte";
	fixture_t f = {.source = source, .source_size = sizeof(source) - 1,
		.receive_limit = 7, .write_limit = 3};
	ftpvita_store_result_t r = transfer(&f);
	assert(strcmp(r.stage, "complete") == 0);
	assert(r.bytes_received == sizeof(source) - 1);
	assert(r.bytes_written == sizeof(source) - 1);
	assert(f.destination_position == sizeof(source) - 1);
	assert(memcmp(f.destination, source, sizeof(source) - 1) == 0);

	f = (fixture_t){.source = source, .source_size = sizeof(source) - 1,
		.fail_receive_call = 2, .receive_failure = -123, .net_error = 35};
	r = transfer(&f);
	assert(strcmp(r.stage, "receive") == 0 && r.code == -123);
	assert(r.net_errno == 35 && r.bytes_received == 8 && r.bytes_written == 8);

	f = (fixture_t){.source = source, .source_size = sizeof(source) - 1,
		.fail_write_call = 2, .write_failure = -456, .write_limit = 3};
	r = transfer(&f);
	assert(strcmp(r.stage, "write") == 0 && r.code == -456);
	assert(r.bytes_received == 8 && r.bytes_written == 3);

	f = (fixture_t){.source = source, .source_size = sizeof(source) - 1,
		.fail_write_call = 1, .write_failure = 0};
	r = transfer(&f);
	assert(strcmp(r.stage, "write") == 0 && r.code == 0);

	unsigned char buffer[8];
	r = ftpvita_store_stream(&f, receive_data, write_file, NULL,
		sizeof(buffer));
	assert(strcmp(r.stage, "range") == 0);
	r = ftpvita_store_stream(&f, receive_data, write_file, buffer, 0);
	assert(strcmp(r.stage, "range") == 0);

	puts("ftpvita STOR partial I/O/error accounting: ok");
	return 0;
}
