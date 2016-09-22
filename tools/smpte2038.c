/* Copyright (c) 2016 Kernel Labs Inc. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <libgen.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <libklvanc/vanc.h>
#include "udp.h"
#include "url.h"

#include "version.h"

#define DEFAULT_FIFOSIZE 1048576
#define MAX_PES_SIZE 16384
#define DEFAULT_PID 0x80

typedef void (*pes_extractor_callback)(void *cb_context, unsigned char *buf, int byteCount);
struct pes_extractor_s
{
	uint16_t pid;
	uint8_t *buf;
	unsigned int buf_size;	/* Complete size of the buf allocation */
	unsigned int buf_used;	/* Amount used, usually less than buf_size */
	unsigned int pes_length;

	int packet_size;
	int appending;
	void *cb_context;
	pes_extractor_callback cb;
};

static struct app_context_s
{
	int verbose;
	int running;
	char *input_url;
	struct url_opts_s *i_url;
	unsigned int pid;

	struct iso13818_udp_receiver_s *udprx;
	struct pes_extractor_s pe;
} app_context;

static struct app_context_s *ctx = &app_context;

static void hexdump(unsigned char *buf, unsigned int len, int bytesPerRow /* Typically 16 */)
{
        for (unsigned int i = 0; i < len; i++)
                printf("%02x%s", buf[i], ((i + 1) % bytesPerRow) ? " " : "\n");
        printf("\n");
}

/* Convert PES data into a series of TS packets */
static int packetizer(uint8_t *buf, unsigned int byteCount, uint8_t **pkts, uint32_t *packetCount, int packetSize, uint8_t *cc, uint16_t pid)
{
	if ((!buf) || (byteCount == 0) || (!pkts) || (!packetCount) || (packetSize != 188) || (!cc) || (pid > 0x1fff))
		return -1;

	unsigned int offset = 0;

	int max = packetSize - 4;
	int packets = ((byteCount / max) + 1) * packetSize;
	int cnt = 0;

	uint8_t *arr = calloc(packets, packetSize);

	unsigned int rem = byteCount - offset;
	while (rem) {
		if (rem > max)
			rem = max;

		uint8_t *p = arr + (cnt * packetSize);
		*(p + 0) = 0x47;
		*(p + 1) = pid >> 8;
		*(p + 2) = pid;
		*(p + 3) = 0x10 | (*cc & 0x0f);
		memcpy(p + 4, buf + offset, rem);
		memset(p + 4 + rem, 0xff, packetSize - rem - 4);
		offset += rem;
		(*cc)++;

		if (cnt++ == 0)
			*(p + 1) |= 0x40; /* PES Header */

		rem = byteCount - offset;
	}

	*pkts = arr;
	*packetCount = cnt;
	return 0;
}

/* When the PES extractor has demultiplexed all data, we're call with the
 * entire PES array. Parse it, dump it to console.
 */
pes_extractor_callback pes_cb(void *cb_context, unsigned char *buf, int byteCount)
{
	if (ctx->verbose) {
		printf("%s()\n", __func__);
		if (ctx->verbose > 1)
			hexdump(buf, byteCount, 16);
	}

	/* Parse the PES section, like any other tool might. */
	struct smpte2038_anc_data_packet_s *result = 0;
	smpte2038_parse_section(buf, byteCount, &result);
	smpte2038_smpte2038_anc_data_packet_dump(result);

	/* TODO: Push the vanc into the VANC processor */

	return 0;
}

/* PES Extractor mechanism, so convert MULTIPLE TS packets containing PES VANC, into PES array. */
static int pe_init(struct pes_extractor_s *pe, void *user_context, pes_extractor_callback cb, uint16_t pid)
{
	memset(pe, 0, sizeof(*pe));
	pe->buf = (uint8_t *)calloc(MAX_PES_SIZE, 1);
	if (!pe->buf)
		return -1;

	pe->pid = pid;
	pe->buf_size = MAX_PES_SIZE;
	pe->cb_context = user_context;
	pe->cb = cb;
	pe->packet_size = 188;

	return 0;
}

static void pe_processPacket(struct pes_extractor_s *pe, unsigned char *pkt, int len)
{
        int section_offset = 4;
#if 0
	if (*(pkt + 1) & 0x40)
                section_offset++;
#endif

        unsigned char adaption = (*(pkt + 3) >> 4) & 0x03;
        if ((adaption == 2) || (adaption == 3)) {
                if (section_offset == 4)
                        section_offset++;
                section_offset += *(pkt + 4);
        }

        if ((pe->appending == 0) &&
		(*(pkt + 1) & 0x40) &&
		(*(pkt + section_offset + 0) == 0) &&
		(*(pkt + section_offset + 1) == 0) &&
		(*(pkt + section_offset + 2) == 1) &&
		(*(pkt + section_offset + 3) == 0xbd)
	) {
                pe->buf_used = 0;
                pe->appending = 1;
                pe->pes_length = *(pkt + section_offset + 4) << 8 | *(pkt + section_offset + 5);
		pe->pes_length += 6; /* Header */
#if 1
                printf("%s() pes_length = %d (0x%x)\n", __func__, pe->pes_length, pe->pes_length);
                printf("%s() section_offset = %d (0x%x)\n", __func__, section_offset, section_offset);
#endif
        }
        if (!pe->appending)
                return;

        int clen = pe->pes_length - pe->buf_used + 3; /* 3 = tableid + length fields, don't forget these */
        if (clen > (len - section_offset))
                clen = len - section_offset;
        memcpy(pe->buf + pe->buf_used, pkt + section_offset, clen);
        pe->buf_used += clen;

	printf("pe->pes_length = %x pe->buf_used = %x\n", pe->pes_length, pe->buf_used);

        if (pe->pes_length > pe->buf_used) {
                return; /* More data required */
        }

        printf("Buffer %d bytes long, section %d bytes long, %02x %02x %02x\n",
                pe->buf_used,
                pe->pes_length,
                *(pe->buf + 0),
                *(pe->buf + 1),
                *(pe->buf + 2));

	if (pe->cb)
		pe->cb(pe->cb_context, pe->buf, pe->pes_length);
}

static size_t pe_push(struct pes_extractor_s *pe, unsigned char *pkt, int packetCount)
{
        if ((!pe) || (packetCount < 1) || (!pkt))
                return 0;

        for (int i = 0; i < packetCount; i++) {
		uint16_t pid = ((*(pkt + 1) << 8) | *(pkt + 2)) & 0x1fff;
                if (pid == pe->pid) {
                        pe_processPacket(pe, pkt + (i * pe->packet_size), pe->packet_size);
                }
        }
        return packetCount;
}


#include "bitstream.h"
static void smpte2038_generate_sample_708B_packet(struct app_context_s *ctx)
{
	/* This is a fully formed 708B VANC message. We'll bury
	 * this inside a SMPTE2038 wrapper and prepare a TS packet
	 * that could be used for sample/test data.
	 */
	unsigned short arr[] = {
		0x000, 0x3ff, 0x3ff, 0x161, 0x101, 0x152, 0x296, 0x269,
		0x152, 0x14f, 0x277, 0x2b8, 0x1ad, 0x272, 0x1f4, 0x2fc,
		0x180, 0x180, 0x1fd, 0x180, 0x180, 0x2fa, 0x200, 0x200,
		0x2fa, 0x200, 0x200, 0x2fa, 0x200, 0x200, 0x2fa, 0x200,
		0x200, 0x2fa, 0x200, 0x200, 0x2fa, 0x200, 0x200, 0x2fa,
		0x200, 0x200, 0x2fa, 0x200, 0x200, 0x2fa, 0x200, 0x200,
		0x2fa, 0x200, 0x200, 0x2fa, 0x200, 0x200, 0x2fa, 0x200,
		0x200, 0x2fa, 0x200, 0x200, 0x2fa, 0x200, 0x200, 0x2fa,
		0x200, 0x200, 0x2fa, 0x200, 0x200, 0x2fa, 0x200, 0x200,
		0x2fa, 0x200, 0x200, 0x173, 0x2d1, 0x1e0, 0x200, 0x200,
		0x200, 0x200, 0x200, 0x200, 0x274, 0x2b8, 0x1ad, 0x194,
		0x1b4, /* Checksum */
	};

	/* See smpte 2038-2008 - Page 5, Table 2 for description. */
	wBitStream *bs  = bitstream_new();

	uint8_t buf[8192] = { 0 };
	bitstream_attach(bs, buf, sizeof(buf));

	/* PES Header - Bug: bitstream can't write 32bit values */
	bitstream_write_bits(bs, 1, 24);	/* packet_start_code_prefix */
	bitstream_write_bits(bs, 0xBD, 8);	/* stream_id */
	bitstream_write_bits(bs, 0, 16);	/* PES_packet_length */
	bitstream_write_bits(bs, 2, 2);		/* '10' fixed value */
	bitstream_write_bits(bs, 0, 2);		/* PES_scrambling_control (not scrambled) */
	bitstream_write_bits(bs, 0, 1);		/* PES_priority */
	bitstream_write_bits(bs, 1, 1);		/* data_alignment_indicator (aligned) */
	bitstream_write_bits(bs, 0, 1);		/* copyright (not-copyright) */
	bitstream_write_bits(bs, 0, 1);		/* original-or-copy (copy) */
	bitstream_write_bits(bs, 2, 2);		/* PTS_DTS_flags (PTS Present) */
	bitstream_write_bits(bs, 0, 1);		/* ESCR_flag (not present) */
	bitstream_write_bits(bs, 0, 1);		/* ES_RATE_flag (not present) */
	bitstream_write_bits(bs, 0, 1);		/* DSM_TRICK_MODE_flag (not present) */
	bitstream_write_bits(bs, 0, 1);		/* additional_copy_info_flag (not present) */
	bitstream_write_bits(bs, 0, 1);		/* PES_CRC_flag (not present) */
	bitstream_write_bits(bs, 0, 1);		/* PES_EXTENSION_flag (not present) */
	bitstream_write_bits(bs, 5, 8);		/* PES_HEADER_DATA_length */
	bitstream_write_bits(bs, 2, 4);		/* '0010' fixed value */

	uint64_t pts = 0;
	bitstream_write_bits(bs, (pts >> 30), 3);	/* PTS[32:30] */
	bitstream_write_bits(bs, 1, 1);			/* marker_bit */
	bitstream_write_bits(bs, (pts >> 15) & 0xefff, 15);	/* PTS[29:15] */
	bitstream_write_bits(bs, 1, 1);			/* marker_bit */
	bitstream_write_bits(bs, (pts & 0xefff), 15);		/* PTS[14:0] */
	bitstream_write_bits(bs, 1, 1);			/* marker_bit */

	int lineCount = 8;
	for (int i = 0; i < lineCount; i++) {
		/* VANC Payload */
		bitstream_write_bits(bs, 0, 6);		/* fixed value '000000' */
		bitstream_write_bits(bs, 0, 1);		/* c_not_y_channel_flag (HD luminance) */
		bitstream_write_bits(bs, 9, 11);	/* line_number (9) */
		bitstream_write_bits(bs, 0, 12);	/* horizonal_offset (0 words from SAV) */
		bitstream_write_bits(bs, arr[3], 10);	/* DID */
		bitstream_write_bits(bs, arr[4], 10);	/* SDID */
		bitstream_write_bits(bs, arr[5], 10);	/* data_count */
		for (int i = 6; i < (sizeof(arr) / sizeof(unsigned short)); i++) {
			/* This data_count AND checksum */
			bitstream_write_bits(bs, arr[i], 10);
		}

		/* Byte alignment stuffing */
		while (bs->position % 8) {
			bitstream_write_bits(bs, 1, 1);	/* Stuffing byte */
		}
	}

	/* Flush the remaining bits out into buffer, else we lose up to
	 * the last 32 bits that are cached in the bitstream implementation.
	 */
	bitstream_flush(bs);

	/* Now updated the PES packet length */
	int len = bitstream_getlength_bytes(bs) - 6;
	bs->buffer[4] = (len >> 8) & 0xff;
	bs->buffer[5] = len & 0xff;

	/* The PES is ready. Les save a file copy. */
	//hexdump(buf, bitstream_getlength_bytes(bs), 16);
	bitstream_file_save(bs, "/tmp/bitstream-scte2038-EIA708B.raw");

	/* Now, also produce a transport packet, for pid */
	uint8_t section[8192];
	int section_length = bitstream_getlength_bytes(bs);
	memset(section, 0xff, sizeof(section));
	memcpy(section, bs->buffer, section_length);

	uint8_t *pkts = 0;
	uint32_t packetCount = 0;
	uint8_t cc = 0;
	packetizer(section, section_length, &pkts, &packetCount, 188, &cc, ctx->pid);
	for (uint32_t i = 0; i < packetCount; i++) {
		//hexdump(pkts + (i * 188), 188, 16);
	}
	FILE *fh = fopen("/tmp/bitstream-scte2038-EIA708B.ts", "wb");
	if (fh) {
		fwrite(pkts, packetCount, 188, fh);
		fclose(fh);
	}

	/* Test the PES extractor */
	struct pes_extractor_s pe;
	pe_init(&pe, 0, (pes_extractor_callback)pes_cb, ctx->pid);
	pe_push(&pe, pkts, packetCount);

	free(pkts);

#if 0
	/* Parse the PES section, like any other tool might. */
	struct smpte2038_anc_data_packet_s *result = 0;
	smpte2038_parse_section(section, bitstream_getlength_bytes(bs), &result);
	smpte2038_smpte2038_anc_data_packet_dump(result);
#endif

	bitstream_free(bs);
}

static tsudp_receiver_callback udp_cb(void *userContext, unsigned char *buf, int byteCount)
{
	struct app_context_s *ctx = userContext;
	if (ctx->verbose) {
		printf("%s() pushing %d bytes\n", __func__, byteCount);
		if (ctx->verbose > 1)
			hexdump(buf, byteCount, 16);
	}
	pe_push(&ctx->pe, buf, byteCount / 188);
	return 0;
}

static void signal_handler(int signum)
{
	ctx->running = 0;
}

static int _usage(const char *progname, int status)
{
	fprintf(stderr, COPYRIGHT "\n");
	fprintf(stderr, "Detect and capture SMPTE2038 VANC frames from a UDP transport stream.\n");
	fprintf(stderr, "Usage: %s [OPTIONS]\n"
		"    -i <udp url. Eg. udp://224.0.0.1:5000>\n"
		"    -P <pid 0xNNNN> VANC PID to process (def: 0x%x)\n"
		"    -v Increase verbose level\n"
		"    -g generate sample SMPTE2038 stream and parse it.\n",
	basename((char *)progname),
	DEFAULT_PID
	);

	exit(status);
}

static int _main(int argc, char *argv[])
{
	int opt;
	int exitStatus = 0;
	ctx->running = 1;
	ctx->pid = DEFAULT_PID;
	ctx->verbose = 0;

	while ((opt = getopt(argc, argv, "?ghi:P:v")) != -1) {
		switch (opt) {
		case 'g':
			smpte2038_generate_sample_708B_packet(ctx);
			exit(0);
			break;
		case 'i':
			ctx->input_url = optarg;
			if (url_parse(ctx->input_url, &ctx->i_url) < 0) {
				_usage(argv[0], 1);
                        }
			break;
                case 'P':
                        if ((sscanf(optarg, "0x%x", &ctx->pid) != 1) || (ctx->pid > 0x1fff))
				_usage(argv[0], 1);
                        break;
		case 'v':
			ctx->verbose++;
			break;
		case '?':
		case 'h':
			_usage(argv[0], 0);
		}
	}

	if (ctx->input_url == NULL) {
		fprintf(stderr, "Missing mandatory -i option\n");
		_usage(argv[0], 1);
	}

	pe_init(&ctx->pe, ctx, (pes_extractor_callback)pes_cb, ctx->pid);

        int fs = DEFAULT_FIFOSIZE;
	if (ctx->i_url->has_fifosize)
		fs = ctx->i_url->fifosize;

	if (iso13818_udp_receiver_alloc(&ctx->udprx, fs,
		ctx->i_url->hostname, ctx->i_url->port, (tsudp_receiver_callback)udp_cb, ctx, 0) < 0) {
		fprintf(stderr, "Unable to allocate a UDP Receiver for %s:%d\n",
		ctx->i_url->hostname, ctx->i_url->port);
		goto no_mem;
	}

	/* Add a multicast NIC if reqd. */
	if (ctx->i_url->has_ifname) {
		iso13818_udp_receiver_join_multicast(ctx->udprx, ctx->i_url->ifname);
	}

	signal(SIGINT, signal_handler);

	iso13818_udp_receiver_thread_start(ctx->udprx);
	while (ctx->running) {
		usleep(100 * 1000);
	}
	iso13818_udp_receiver_free(&ctx->udprx);

no_mem:
	return exitStatus;
}

int smpte2038_main(int argc, char *argv[])
{
	return _main(argc, argv);
}
